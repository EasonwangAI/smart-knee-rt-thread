# 本次版本与来源

整理日期：2026-10-06。文件名中的 v8 是原项目名，不代表各端软件都使用版本号 8。

| 部分 | 本次采用的版本 | 入口 |
| --- | --- | --- |
| STM32 | 本次新提供的工程压缩包源码；内扣 +3° / 外扩 -40°，确认配置 130 ms | [工程](../firmware/rt-thread-studio-project/) |
| ESP32 | v1.3.2 data-link-fix，Wi-Fi/HTTP | [Arduino 工程](../firmware/esp32/KneePad_ESP32_AP/) |
| Android 安装包 | v1.2.4，versionCode 8，Android 7.0+ | [下载 APK](../software/android/releases/SmartKneePad-v1.2.4-emg-contact-action-latch.apk?raw=true) |
| Android 开发源码 | v1.5.0，versionCode 17；与上述 APK 不同 | [源码](../software/android/source-v1.5.0/) |
| STM32 归档二进制 | Debug/rtthread.bin，466,020 字节，原样提供 | [BIN](../firmware/releases/stm32-archive-20261006.bin?raw=true) |

## SHA-256

| 文件 | SHA-256 |
| --- | --- |
| 原 STM32 压缩包 `test_pro3_v8_dbui_kneeout (1).zip` | `F231E5A15D8CB6D86C7403ADE76BA6B890766898D1FEFFC8990C7CFFE5106218` |
| ESP32 v1.3.2 源码 | `61F158ACE50356C378CB5A0F782B4DDC7817FFD3613416E015403206D6C5E1AF` |
| Android v1.2.4 APK | `F46A3A4E4D4511F1E9A37E2179393D097B367B276085B9C1F5CCC635F7C99D8B` |
| 原归档 Debug 固件 | `D6C8D81F84C38564A522FCEA2D3233E775EFE03C22D1169216E9996B1FA07B30` |

本次 APK 与工程归档 `outputs/final_v135-emg-contact-action-latch` 内的同名 APK 哈希一致；单独提供的 ESP32 文件与归档内当前 ESP32 文件也一致。

## 使用关系

提供的 v1.2.4 APK 与 ESP32 v1.3.2 采用 Wi-Fi 热点/HTTP 接口。可以按 [复现指南](REPRODUCE.md) 安装、连接和检查数据。

Android v1.5.0 是额外的开发源码，含 BLE 客户端和 AI 设置；当前 ESP32 没有 BLE 服务。没有把 v1.5.0 源码改名成 v1.2.4，也没有声称提供的 APK 来自该源码。

本次核心程序文件保持原样，仅进行目录整理、文档更新、APK 后缀恢复和运行数据库公开副本的姓名清理。未重新训练模型、改变阈值或刷新周期。

## 实际行为注意

- 总训练次数包含完成但分类 UNKNOWN 的动作，步行采用独立次数。
- 压力 ADC 可采集，姿态结论当前来自双 MPU6050。
- 演示用疲劳门控仍在，详见 [DEMO_BEHAVIOR.md](DEMO_BEHAVIOR.md)。
- 内扣阈值源码当前是 +3°，原历史说明中的 +5°、+50° 等不代表这次版本。
- 固件未在此次整理中重新构建、烧录或进行佩戴验证。

[机器可读版本清单](VERSIONS.json)
