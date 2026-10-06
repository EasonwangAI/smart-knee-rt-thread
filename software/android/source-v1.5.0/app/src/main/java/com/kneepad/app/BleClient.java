package com.kneepad.app;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * 蓝牙连接客户端：通过 ESP32-S3 的 BLE 通道（Nordic UART Service）与护膝通信。
 *
 * 数据协议（需 ESP32 固件配合实现）：
 *  - APP -> 设备（写 RX 特征，行分隔）：
 *      P,&lt;配对码&gt;         配对验证
 *      GETPROFILE         读取档案列表
 *      SELECT,&lt;id&gt;       选择档案
 *      SAVE,{json}        保存档案
 *      DELETE,&lt;id&gt;       删除档案
 *      START / END        开始 / 结束训练
 *      GETLIVE            主动拉取一次实时数据
 *  - 设备 -> APP（TX 特征通知，行分隔）：
 *      D,{json}           实时数据推送（每 500ms，内容与 /api 相同）
 *      A,{json}           命令应答（按发送顺序返回）
 */
final class BleClient {
    interface Listener {
        void onLiveJson(String json);
        void onState(String message);
        void onError(String message);
    }

    static final String DEVICE_NAME_PREFIX = "KneePad";
    static final UUID NUS_SERVICE_UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    static final UUID NUS_RX_UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
    static final UUID NUS_TX_UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e");

    private static final long SCAN_TIMEOUT_MS = 12000L;

    private final Context context;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Listener listener;
    private final ArrayDeque<ApiClient.Callback<org.json.JSONObject>> pending = new ArrayDeque<>();

    private BluetoothLeScanner scanner;
    private ScanCallback activeScanCallback;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rxCharacteristic;
    private boolean connected;
    private String latestLiveJson;
    private final StringBuilder rxBuffer = new StringBuilder();

    BleClient(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
    }

    boolean isConnected() {
        return connected;
    }

    String latestLiveJson() {
        return latestLiveJson;
    }

    boolean hasAdapter() {
        BluetoothManager manager = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        return manager != null && manager.getAdapter() != null && manager.getAdapter().isEnabled();
    }

    void enableBluetooth() {
        BluetoothManager manager = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (manager == null || manager.getAdapter() == null) return;
        BluetoothAdapter adapter = manager.getAdapter();
        if (!adapter.isEnabled()) adapter.enable();
    }

    void scan(final ApiClient.Callback<List<ScanResult>> callback) {
        BluetoothManager manager = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (manager == null || manager.getAdapter() == null) {
            callback.onError("本机不支持蓝牙");
            return;
        }
        BluetoothAdapter adapter = manager.getAdapter();
        if (!adapter.isEnabled()) {
            callback.onError("请先开启手机蓝牙");
            return;
        }
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            callback.onError("无法启动蓝牙扫描");
            return;
        }
        List<ScanResult> results = new ArrayList<>();
        List<String> seen = new ArrayList<>();
        activeScanCallback = new ScanCallback() {
            @Override
            public void onScanResult(int callbackType, ScanResult result) {
                String name = result.getDevice().getName();
                if (name == null) name = "";
                if (!name.contains(DEVICE_NAME_PREFIX) && !name.toUpperCase().contains("ESP32")) return;
                if (seen.contains(result.getDevice().getAddress())) return;
                seen.add(result.getDevice().getAddress());
                results.add(result);
                if (listener != null) listener.onState("发现设备 " + name);
            }

            @Override
            public void onScanFailed(int errorCode) {
                stopScan();
                callback.onError("蓝牙扫描失败，请重试");
            }
        };
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        try {
            scanner.startScan(null, settings, activeScanCallback);
        } catch (SecurityException exception) {
            callback.onError("缺少蓝牙扫描权限，请授予位置/附近设备权限");
            return;
        }
        handler.postDelayed(() -> {
            stopScan();
            if (results.isEmpty()) callback.onError("未发现 KneePad 护膝设备，请确认已上电并靠近手机");
            else callback.onSuccess(results);
        }, SCAN_TIMEOUT_MS);
    }

    void stopScan() {
        if (scanner != null && activeScanCallback != null) {
            try {
                scanner.stopScan(activeScanCallback);
            } catch (Exception ignored) {
            }
        }
        activeScanCallback = null;
        scanner = null;
    }

    void connect(BluetoothDevice device, final ApiClient.Callback<Boolean> callback) {
        if (device == null) {
            callback.onError("设备为空");
            return;
        }
        if (gatt != null) {
            try {
                gatt.disconnect();
                gatt.close();
            } catch (Exception ignored) {
            }
            gatt = null;
        }
        connected = false;
        rxCharacteristic = null;
        try {
            gatt = device.connectGatt(context, false, new BluetoothGattCallback() {
                @Override
                public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
                    handler.post(() -> {
                        if (newState == BluetoothProfile.STATE_CONNECTED) {
                            if (listener != null) listener.onState("蓝牙已连接，正在发现服务…");
                            g.discoverServices();
                        } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                            connected = false;
                            if (listener != null) listener.onState("蓝牙已断开");
                        }
                    });
                }

                @Override
                public void onServicesDiscovered(BluetoothGatt g, int status) {
                    handler.post(() -> {
                        if (status != BluetoothGatt.GATT_SUCCESS) {
                            if (callback != null) callback.onError("服务发现失败");
                            return;
                        }
                        BluetoothGattService service = g.getService(NUS_SERVICE_UUID);
                        if (service == null) {
                            if (callback != null) callback.onError("设备未启用 BLE 透传服务（需刷 BLE 固件）");
                            return;
                        }
                        BluetoothGattCharacteristic tx = service.getCharacteristic(NUS_TX_UUID);
                        if (tx == null) {
                            if (callback != null) callback.onError("未找到 BLE 数据通道");
                            return;
                        }
                        try {
                            g.setCharacteristicNotification(tx, true);
                            BluetoothGattDescriptor descriptor = tx.getDescriptor(
                                    UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"));
                            if (descriptor != null) {
                                descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                                g.writeDescriptor(descriptor);
                            }
                        } catch (SecurityException exception) {
                            if (callback != null) callback.onError("缺少蓝牙连接权限");
                            return;
                        }
                        rxCharacteristic = service.getCharacteristic(NUS_RX_UUID);
                        connected = true;
                        if (listener != null) listener.onState("BLE 数据通道已就绪");
                        if (callback != null) callback.onSuccess(true);
                    });
                }

                @Override
                public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic characteristic) {
                    byte[] value;
                    try {
                        value = characteristic.getValue();
                    } catch (Exception exception) {
                        return;
                    }
                    if (value == null) return;
                    String chunk = new String(value, java.nio.charset.StandardCharsets.UTF_8);
                    handler.post(() -> consumeChunk(chunk));
                }
            });
        } catch (SecurityException exception) {
            if (callback != null) callback.onError("缺少蓝牙连接权限");
        }
    }

    private void consumeChunk(String chunk) {
        rxBuffer.append(chunk);
        String text = rxBuffer.toString();
        int index;
        while ((index = text.indexOf('\n')) >= 0) {
            String line = text.substring(0, index).trim();
            text = text.substring(index + 1);
            if (line.isEmpty()) continue;
            if (line.startsWith("D,")) {
                latestLiveJson = line.substring(2);
                if (listener != null) listener.onLiveJson(latestLiveJson);
            } else if (line.startsWith("A,")) {
                ApiClient.Callback<org.json.JSONObject> request = pending.pollFirst();
                if (request != null) {
                    try {
                        request.onSuccess(new org.json.JSONObject(line.substring(2)));
                    } catch (Exception exception) {
                        request.onError("设备应答格式错误");
                    }
                }
            } else if (line.startsWith("E,")) {
                ApiClient.Callback<org.json.JSONObject> request = pending.pollFirst();
                if (request != null) request.onError(line.substring(2));
            }
        }
        rxBuffer.setLength(0);
        rxBuffer.append(text);
    }

    /**
     * 发送一条命令，等待设备按顺序应答（A,{json} 或 E,{error}）。
     */
    void sendCommand(String command, ApiClient.Callback<org.json.JSONObject> callback) {
        if (!connected || gatt == null || rxCharacteristic == null) {
            if (callback != null) callback.onError("蓝牙尚未连接");
            return;
        }
        pending.addLast(callback == null ? new NoopCallback() : callback);
        rxCharacteristic.setValue(command + "\n");
        try {
            gatt.writeCharacteristic(rxCharacteristic);
        } catch (SecurityException exception) {
            pending.pollLast();
            if (callback != null) callback.onError("缺少蓝牙写入权限");
        }
    }

    void disconnect() {
        stopScan();
        handler.removeCallbacksAndMessages(null);
        if (gatt != null) {
            try {
                gatt.disconnect();
                gatt.close();
            } catch (Exception ignored) {
            }
            gatt = null;
        }
        connected = false;
        pending.clear();
    }

    private static final class NoopCallback implements ApiClient.Callback<org.json.JSONObject> {
        @Override public void onSuccess(org.json.JSONObject value) {
        }

        @Override public void onError(String message) {
        }
    }
}
