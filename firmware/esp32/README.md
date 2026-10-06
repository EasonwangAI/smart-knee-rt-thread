# ESP32-S3 无线固件 v1.3.2

打开 [KneePad_ESP32_AP/KneePad_ESP32_AP.ino](KneePad_ESP32_AP/KneePad_ESP32_AP.ino)。Arduino 工程文件夹和 `.ino` 文件同名，方便直接编译。

该文件与提供的 `KneePad_ESP32_AP-v1.3.2-data-link-fix.ino` 内容完全一致，仅调整存放位置和文件名。

## 当前连接参数

| 参数 | 值 |
| --- | --- |
| 串口 | HardwareSerial(1)，115200，8N1 |
| ESP32 RX | GPIO18，连接 STM32 PA9/TX |
| ESP32 TX | GPIO17，连接 STM32 PA10/RX，用于请求数据流 |
| Wi-Fi 热点 | `KneePad_ESP32` |
| 热点密码 | `12345678` |
| 配对码 | `2580` |
| 默认网页地址 | `http://192.168.4.1/` |

V1.3.2 在启动以及数据过期时向 STM32 发送 `knee_start` 和 `pressure_stream on`。建议保留 TX 回传线；如果只接收数据，需要确认 STM32 已主动输出两类帧。

包含用户档案、训练开始/结束、疲劳分数、动作质量、电极接触、姿态状态以及数据时效性显示。此固件仅实现 Wi-Fi/HTTP，未实现 BLE GATT 服务。

[接线与调试](../../docs/REPRODUCE.md) · [协议](../../docs/PROTOCOL.md)
