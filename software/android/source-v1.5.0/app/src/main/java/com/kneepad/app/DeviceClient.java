package com.kneepad.app;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONObject;

import java.util.LinkedHashMap;
import java.util.Map;

import com.kneepad.app.ApiClient.Callback;

/**
 * 设备连接门面：统一封装 Wi-Fi（ApiClient）与 蓝牙（BleClient）两种连接方式，
 * 对外暴露与 ApiClient 相同的方法签名，MainActivity 无需关心底层通道。
 */
final class DeviceClient {
    private static final String PREFS = "kneepad_connection";
    private static final String KEY_PAIR_CODE = "pair_code";

    private final Context context;
    private final ApiClient wifi;
    private final BleClient ble;
    private volatile boolean bleMode;
    private volatile boolean bleDataReady;

    DeviceClient(Context context, BleClient.Listener bleListener) {
        this.context = context.getApplicationContext();
        wifi = new ApiClient(this.context);
        ble = new BleClient(this.context, bleListener);
    }

    BleClient ble() {
        return ble;
    }

    boolean bleMode() {
        return bleMode;
    }

    void setBleMode(boolean value) {
        bleMode = value;
        bleDataReady = false;
    }

    boolean bleDataReady() {
        return bleDataReady;
    }

    String pairCode() {
        String saved = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_PAIR_CODE, "");
        // 设备默认配对码 2580 自动使用，用户无需手动输入
        return saved.isEmpty() ? "2580" : saved;
    }

    void clearPairCode() {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(KEY_PAIR_CODE).apply();
    }

    void verifyPairCode(String code, Callback<Boolean> callback) {
        if (bleMode) {
            final String trimmed = code == null ? "" : code.trim();
            ble.sendCommand("P," + trimmed, new Callback<JSONObject>() {
                @Override public void onSuccess(JSONObject value) {
                    boolean ok = value.optBoolean("ok", false);
                    if (ok) context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                            .edit().putString(KEY_PAIR_CODE, trimmed).apply();
                    callback.onSuccess(ok);
                }

                @Override public void onError(String message) {
                    callback.onError(message);
                }
            });
            return;
        }
        wifi.verifyPairCode(code, callback);
    }

    void loadProfiles(final Callback<ProfileState> callback) {
        if (bleMode) {
            ble.sendCommand("GETPROFILE", new Callback<JSONObject>() {
                @Override public void onSuccess(JSONObject value) {
                    ProfileState state = new ProfileState();
                    state.active = value.optBoolean("active", false);
                    org.json.JSONArray items = value.optJSONArray("profiles");
                    if (items != null) {
                        for (int i = 0; i < items.length(); i++) {
                            state.profiles.add(UserProfile.fromJson(items.optJSONObject(i)));
                        }
                    }
                    JSONObject active = value.optJSONObject("profile");
                    if (active != null) state.profile = UserProfile.fromJson(active);
                    callback.onSuccess(state);
                }

                @Override public void onError(String message) {
                    callback.onError(message);
                }
            });
            return;
        }
        wifi.loadProfiles(callback);
    }

    void selectProfile(int id, Callback<Boolean> callback) {
        if (bleMode) {
            ble.sendCommand("SELECT," + id, boolCallback(callback));
            return;
        }
        wifi.selectProfile(id, callback);
    }

    void saveProfile(UserProfile profile, Callback<Boolean> callback) {
        if (bleMode) {
            try {
                JSONObject body = new JSONObject();
                body.put("id", profile.id);
                body.put("name", profile.name == null ? "" : profile.name);
                body.put("gender", profile.gender == null ? "未设置" : profile.gender);
                body.put("age", profile.age);
                body.put("height_cm", profile.heightCm);
                body.put("weight_kg", profile.weightKg);
                body.put("exercise_habit", profile.habit == null ? "偶尔运动" : profile.habit);
                body.put("goal", profile.goal == null ? "日常锻炼" : profile.goal);
                body.put("injury", profile.injury == null ? "无" : profile.injury);
                ble.sendCommand("SAVE," + body.toString(), boolCallback(callback));
            } catch (Exception exception) {
                callback.onError("档案数据编码失败");
            }
            return;
        }
        wifi.saveProfile(profile, callback);
    }

    void deleteProfile(int id, Callback<Boolean> callback) {
        if (bleMode) {
            ble.sendCommand("DELETE," + id, boolCallback(callback));
            return;
        }
        wifi.deleteProfile(id, callback);
    }

    void startTraining(Callback<Boolean> callback) {
        if (bleMode) {
            ble.sendCommand("START", boolCallback(callback));
            return;
        }
        wifi.startTraining(callback);
    }

    void endTraining(Callback<Boolean> callback) {
        if (bleMode) {
            ble.sendCommand("END", boolCallback(callback));
            return;
        }
        wifi.endTraining(callback);
    }

    void loadLiveData(Callback<LiveData> callback) {
        if (bleMode) {
            String json = ble.latestLiveJson();
            if (json == null || json.trim().isEmpty()) {
                callback.onError("等待蓝牙数据");
                return;
            }
            bleDataReady = true;
            try {
                callback.onSuccess(LiveData.fromJson(new JSONObject(json)));
            } catch (Exception exception) {
                callback.onError("蓝牙数据解析失败");
            }
            return;
        }
        wifi.loadLiveData(callback);
    }

    void shutdown() {
        wifi.shutdown();
        ble.disconnect();
    }

    private static Callback<JSONObject> boolCallback(final Callback<Boolean> callback) {
        return new Callback<JSONObject>() {
            @Override public void onSuccess(JSONObject value) {
                callback.onSuccess(value.optBoolean("ok", false));
            }

            @Override public void onError(String message) {
                callback.onError(message);
            }
        };
    }
}
