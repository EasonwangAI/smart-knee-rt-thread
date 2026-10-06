# 固件入口

| 模块 | 目录 | 开发工具 |
| --- | --- | --- |
| STM32 护膝主控 | [rt-thread-studio-project](rt-thread-studio-project/) | RT-Thread Studio，ARM GCC |
| ESP32-S3 无线桥 | [esp32/KneePad_ESP32_AP](esp32/KneePad_ESP32_AP/) | Arduino IDE，Espressif Arduino-ESP32 |
| 归档内提供的板端固件 | [releases](releases/) | 按实际 STM32F407VE 硬件烧录 |

STM32 工程的原始项目名称仍为 `test_pro3_v8_dbui_kneeout`。导入时请使用单独的工作区或新目录，避免与旧同名工程混淆。

本次保留源码原始内容。`rt-thread-studio-project/python/` 来自仓库初始提交，是历史离线实验脚本，不是新增的手机端或当前动作状态机。

[详细使用方法](../docs/REPRODUCE.md) · [版本清单](../docs/VERSIONS.md)
