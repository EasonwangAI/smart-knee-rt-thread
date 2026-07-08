# mpu_wave - FIT monitor 实时上位机

实时显示 STM32F407 + ADS1292R + MPU6050 输出的 EMG / IMU 数据，并把原始串口流自动存到 log 文件。

## 安装

只需 numpy / matplotlib / pyserial（Anaconda 默认已有 numpy/matplotlib，pyserial 也通常装好）：

```bash
pip install pyserial      # 如果没装
```

## 用法

**1. 先看你有哪些串口：**

```bash
python monitor.py list-ports
```

输出类似：
```
  COM3        USB Serial Port (COM3)
  COM5        Bluetooth ...
```

**2. 连接板子（板子先上电）：**

```bash
python monitor.py COM3
```

或显式指定波特率（默认 115200）：

```bash
python monitor.py COM3 --baud 115200 --plot-window 30
```

打开后会弹一个 4 联子图窗口：

- **EMG 包络 (MAV, mV)**：肌肉激活的强度，附带 `rest_mav`（绿虚线）和 `active_th`（红虚线）参考线
- **EMG 原始波形 (AC, mV)**：滤波后的瞬时 EMG，可以看到肌电脉冲
- **膝关节角度 (deg)**：MPU6050 测的大腿倾角
- **IMU 合成幅度**：加速度 |a| (g) 和角速度 |g|/100 (dps)

右侧状态面板显示：
- 当前 phase / status / motion 状态
- LIVE 行数、wall 时间、rep 计数
- 最新一条 CAL / REP / MSG 事件全文

**3. 数据自动存盘**：

启动时会在 `D:\mpu_wave\session_YYYYMMDD_HHMMSS.log` 创建一个 log 文件，**原样**记录所有串口收到的行。关闭窗口（或 Ctrl+C）即停止录制。

**4. 回放一个 log 文件**（不用接板子，调试用）：

```bash
python monitor.py replay session_20260523_141500.log --speed 2
```

`--speed 2` 表示 2 倍速回放。

## 关键观察点（电极接好坏的判定）

打开 monitor 后，对照下面这张表看 EMG MAV 图：

| 现象 | 含义 |
|---|---|
| MAV 一直 < 1 mV，绷紧大腿时跳到 3-10 mV | ✓ 电极正常，固件正常 |
| MAV 在 5-20 mV 之间一直波动，跟你用力无关 | ✗ 共模噪声大，多半 RLD 参考没接 |
| MAV 单调上升（开始小后越来越大） | ✗ 电极极化漂移，撕下来重贴 |
| MAV 全程几十/几百 mV | ✗ ADS1292R 输入饱和，电极接错或线断 |

如果原始 AC 波形（第 2 张图）能看到清晰的、随着用力出现的肌电"爆发"，那就是电极工作正常了。

## 跟模型分析联动

录完一个 session 后，把 log 文件交给：

```bash
cd D:\RT-ThreadStudio\workspace\test_pro3
python python\session_replay.py D:\mpu_wave\session_xxxxxx.log
```

会跑你的 KneE-PAD 训练 RF 模型，输出每秒预测的动作类别。
