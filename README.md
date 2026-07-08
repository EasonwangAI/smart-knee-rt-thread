# 防损伤智能肌肉护膝（RT-Thread）

本仓库为嵌入式芯片与系统设计竞赛 RT-Thread 赛道作品的开源代码整理版。作品面向运动训练、健身锻炼与康复训练场景，通过腿部肌电采集、膝部姿态辅助判断、疲劳评分、屏幕显示和震动提醒，形成可穿戴式运动状态反馈闭环。

## 项目功能

- 使用 STM32F407VET6 作为主控，基于 RT-Thread 多线程组织板端任务。
- 使用 ADS1292R 与三片贴肤电极采集腿部表面肌电信号。
- 肌电处理按 500 Hz 采样设计，采用 256 点滑动窗口和 64 点步进。
- 对原始肌电进行直流估计、20 Hz 高通、50 Hz 陷波、150 Hz 低通和滑动窗口统计。
- 提取 RMS、MAV、WL、ZC、SSC 等肌电特征，生成 0-100 疲劳评分。
- 使用 MPU6050 以 20 ms 周期读取加速度和角速度，根据膝部倾角辅助划分动作阶段并统计动作次数。
- 使用圆形 TFT 屏显示动作次数、动作类别、疲劳分数、肌电状态和报警状态。
- 当疲劳评分达到阈值并持续多个窗口时，通过震动马达进行触觉提醒。
- 支持串口 LIVE/REP/CAL/RAW 数据输出，Python 上位机可实时绘图、保存日志和进行模型验证。

## 仓库结构

```text
firmware/rt-thread-studio-project/   RT-Thread Studio 板端工程核心代码
software/pc_tools_and_training/      Python 上位机、数据处理、训练与验证脚本
data/sample_capture/                 ADS1292R 示例采集数据
data/training_curated/               精选训练特征、窗口数据和模型文件
docs/                                开源说明、文件说明和复现实验说明
```

## 板端核心代码

主要代码位于 `firmware/rt-thread-studio-project/applications/`：

- `main.c`：系统初始化、RT-Thread 线程、姿态读取、动作阶段、串口输出、屏幕刷新和震动提醒逻辑。
- `emg_pipeline.c/.h`：肌电滤波、滑动窗口、特征提取、校准和疲劳评分。
- `ads1292.c/.h`：ADS1292R SPI 通信、寄存器配置和数据读取。
- `gc9a01.c/.h`：圆形 TFT 屏驱动。

## 上位机与算法代码

主要代码位于 `software/pc_tools_and_training/`：

- `monitor.py`：串口实时监测、波形显示和日志保存。
- `raw_capture.py`：原始串口数据采集。
- `train_fatigue_emg_only.py`：疲劳模型训练。
- `train_action_squat_deadlift_walking.py`：动作模型训练与验证。
- `curate_training_data.py`、`auto_label.py`：训练数据整理和自动标注辅助。
- `predict_fatigue.py`、`dual_predict.py`：模型推理演示。

## 注意事项

本仓库保留本作品原创核心代码、上位机工具、精选训练数据与模型文件。RT-Thread 内核源码、Debug/Build 编译产物、缓存文件和本地 Git 历史未纳入本仓库。若需要重新编译板端工程，请在 RT-Thread Studio 中配置对应 STM32F407VET6 BSP 和依赖环境。
