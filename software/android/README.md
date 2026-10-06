# Android 手机端

这次提供的安装包和压缩包内的开发源码属于不同版本，分别保留。

| 内容 | 路径 | 已核对的版本 |
| --- | --- | --- |
| 可安装 APK | [releases/SmartKneePad-v1.2.4-emg-contact-action-latch.apk](releases/SmartKneePad-v1.2.4-emg-contact-action-latch.apk?raw=true) | `com.kneepad.app`，versionName 1.2.4，versionCode 8 |
| 开发源码 | [source-v1.5.0](source-v1.5.0/) | versionName 1.5.0，versionCode 17 |

APK 原文件后缀为 `.apk.1`，仅恢复标准 `.apk` 后缀；二进制内容未修改。最低 Android API 为 24（Android 7.0），目标 API 为 35。

## 使用提供的 APK

1. 安装 v1.2.4 APK。
2. 连接 ESP32 的 `KneePad_ESP32` 热点，密码 `12345678`。手机提示无互联网时选择保留连接。
3. 打开应用，根据界面选择/创建档案，配对码为 `2580`。
4. 先确认浏览器 `http://192.168.4.1/api` 有新鲜数据，再检查应用数据显示。

## 开发源码 v1.5.0

使用 Android Studio 打开 `source-v1.5.0`，配置 JDK 17 和 Android SDK 35。工程使用 Android Gradle Plugin 8.7.3；包内未包含 Gradle Wrapper，需使用 Android Studio 配置兼容的 Gradle 8.9。`local.properties` 与签名私钥需在开发者机器上配置。

当前源码包含实时显示、档案管理、本地训练记录、CSV 导出、Wi-Fi/BLE 连接入口和 AI 建议设置。AI 接口参数由用户在应用中配置；仓库未携带个人 API 密钥。

BLE 客户端期待 Nordic UART Service，但本次 ESP32 v1.3.2 固件没有提供这个服务。使用当前提交的固件时应选择 Wi-Fi。V1.5.0 未在本次整理中重新构建或验证，不能把 v1.2.4 APK 当作 v1.5.0 构建产物。

V1.2.4 的对应完整源码未在本次归档中找到，不以反编译文件冒充原始源码。[版本与哈希](../../docs/VERSIONS.md)
