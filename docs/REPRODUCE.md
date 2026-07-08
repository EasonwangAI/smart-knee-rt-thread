# 复现说明

## 板端工程

1. 使用 RT-Thread Studio 打开 `firmware/rt-thread-studio-project`。
2. 目标主控为 STM32F407VET6。
3. 按实际硬件连接 ADS1292R、MPU6050、圆形 TFT 屏和震动马达模块。
4. 编译并烧录后，串口会输出 LIVE/REP/CAL/RAW 等数据。

## 上位机

进入 `software/pc_tools_and_training` 后，可按需要运行：

```bash
python monitor.py
python raw_capture.py
python predict_fatigue.py
```

串口号、波特率和模型路径请根据实际电脑环境修改。

## 数据与模型

`data/training_curated` 中包含精选窗口数据、特征数据和训练后的模型文件，可用于复现实验分析。当前模型主要用于课程展示和算法验证，若用于不同佩戴者或更多动作场景，需要重新采集数据并校准。
