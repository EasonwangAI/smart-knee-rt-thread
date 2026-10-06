# 编译、接线与运行

本说明对应 2026-10-06 整理版。[先查看版本对应关系](VERSIONS.md)，再选择要安装的文件。

## 1. STM32 工程

1. 获取完整仓库，使用 RT-Thread Studio 的“导入现有工程”选择 `firmware/rt-thread-studio-project`。
2. 原项目名称为 `test_pro3_v8_dbui_kneeout`。使用独立目录或工作区，避免导入到原有同名旧工程上。
3. 目标芯片为 STM32F407VE，工程内含 RT-Thread 4.1.0、HAL/CMSIS、链接脚本和 `.config`/`rtconfig.h`。开发环境需要安装 ARM GCC 和对应下载器支持。
4. 编译工程，查看新的 Flash/RAM 统计，按自己的 ST-LINK/DAPLink 硬件选择下载配置并烧录。
5. UART1 为 PA9/TX、PA10/RX，默认通信使用 115200、8N1。串口助手先确认能收到 KNEE/PRESS 帧。

SCons 入口随原工程保留，但 `rtconfig.py` 的命令行编译选项很少，本文不承诺直接运行 `scons` 即可替代 RT-Thread Studio 构建。

## 2. 源码中的接线定义

| 模块 | 引脚/总线 |
| --- | --- |
| ADS1292R SPI2 | PB13/SCK、PB14/MISO、PB15/MOSI、PB12/CS |
| ADS1292R 控制 | PB10/RESET、PB11/START、PA8/DRDY |
| 大腿 MPU6050 | 软件 I2C1：PB6/SCL、PB7/SDA，地址 0x68 |
| 小腿 MPU6050 | 软件 I2C2：PB8/SCL、PB9/SDA，地址 0x68 |
| GC9A01 圆屏 | PA5/SCK、PA7/MOSI、PA4/CS、PA3/DC、PA2/RST |
| 震动模块 | PE0/IN，共地，模块供电按实际规格 |
| 内侧/外侧压力模块 | PC0/ADC1_IN10、PC1/ADC1_IN11 |
| STM32 到 ESP32 | PA9/TX 接 ESP32 GPIO18/RX |
| ESP32 到 STM32 | GPIO17/TX 接 PA10/RX |

上表依据源码，不替代实际模块丝印。STM32 ADC 输入电压必须处于允许范围，不能把 5 V 模块输出直接接 ADC。串口与 ADC 连接前确认电平并共地。

## 3. 存储卡匹配库

把 `data/runtime_matching` 的**内容**复制到已挂载介质中的 `smart_knee_runtime_db` 文件夹，确保板端看到：

```text
/smart_knee_runtime_db/runtime_units_normalized.csv
/smart_knee_runtime_db/DB_compatible/reference_features.csv
```

挂载点不同则需调整固件中的 `FATIGUE_DB_RUNTIME_ROOT`，不能仅凭盘符判断板端路径。固件读取 CSV；SQLite 文件供电脑侧使用。

当前匹配实现使用 Top-5，最小有效查询样本条件及 15 秒匹配刷新间隔见 `fatigue_db.c`，并保留内置参考特征作为无文件时的回退。历史训练资料仍在 `data/training_curated` 和 `software/pc_tools_and_training`。

## 4. ESP32-S3

1. Arduino IDE 安装 Espressif Arduino-ESP32 开发板支持。
2. 打开 `firmware/esp32/KneePad_ESP32_AP/KneePad_ESP32_AP.ino`，选择与实际板子一致的 ESP32-S3 配置和端口。
3. 编译并上传本次 v1.3.2 固件。它不依赖额外第三方 BLE 库。
4. ESP32 启动后会请求 `knee_start` 和 `pressure_stream on`；建议接好双向 UART，以便启动恢复命令能到达 STM32。

## 5. 手机

1. 安装 `software/android/releases/SmartKneePad-v1.2.4-emg-contact-action-latch.apk`。
2. 连接热点 `KneePad_ESP32`，密码 `12345678`，保持无互联网的 Wi-Fi 连接。
3. 先打开 `http://192.168.4.1/api`，确认 JSON 中 `age_ms` 持续更新且 KNEE 数据不是长期等待。
4. 打开应用，创建/选择档案，配对码 `2580`，开始训练。
5. 板端启动时按屏幕校准阶段提示操作；双 MPU 的站立校准与肌电静息/主动校准属于不同过程。

手机显示的个人建议阈值不等于 STM32 马达报警条件。疲劳演示门控详见 [DEMO_BEHAVIOR.md](DEMO_BEHAVIOR.md)。

## 6. 排查顺序

- 浏览器也打不开：检查热点连接、地址和手机是否切回移动网络。
- 网页可打开但 `age_ms` 长期很大：检查 PA9 到 GPIO18、共地、串口波特率和 KNEE 输出。
- KNEE 更新但姿态等待：检查小腿 MPU6050、I2C2、PRESS 数据流和校准状态。
- 手机蓝牙找不到数据：此次 ESP32 是 Wi-Fi 固件，选择 Wi-Fi 方式。
- 总数与两类动作之和不同：当前 UNKNOWN 也计入总动作，参见仓库首页说明。

## 本次验证范围

仅验证文件结构、SHA-256、APK 元数据、数据库完整性和通信接口说明；未重新编译三端工程、烧录或进行佩戴测试。`firmware/releases` 中的二进制来自原归档，不是本次新构建的发布固件。
