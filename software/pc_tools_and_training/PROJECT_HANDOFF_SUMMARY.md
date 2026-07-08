# 智能肌肉护膝项目交接总结

更新日期：2026-06-04  
项目目录：`D:\mpu_wave`  
RT-Thread Studio 工程目录：`D:\RT-ThreadStudio\workspace\test_pro3`  
本地备份/镜像工程目录：`D:\mpu_wave\rt_thread_project\test_pro3`

---

## 1. 项目目标

本项目是一个防损伤的“智能肌肉护膝”。

核心目标：

1. 用 `ADS1292R` 采集肌电/类似肌电信号。
2. 用 `MPU6050` 采集膝关节姿态/动作数据。
3. 上位机或板端计算特征：
   - 肌电特征：`MAV`, `RMS`, `WL`, `IEMG`, `VAR`, `ZC`, `SSC`, `MDF`, `MPF` 等。
   - IMU 特征：膝关节角度、加速度模长、角速度模长、摆动幅度等。
4. 实现两个识别功能：
   - 动作识别：目标是 `Squat` 深蹲、`Deadlift` 硬拉、`Walking` 走路。
   - 疲劳识别：`Fresh` 不疲劳、`Fatigued` 疲劳。
5. 当检测到疲劳时，用震动马达提示用户。
6. 后续计划在圆形 TFT 屏幕显示疲劳程度、动作类别和简化波形。

当前阶段重点：

- 方案 D：电脑上位机实时运行完整模型，STM32 负责采集和执行震动。
- 板端模型部署暂时不是当前优先级，因为随机森林模型太大，直接上 STM32F407 不现实。

---

## 2. 硬件组成

当前硬件：

- 主控：`STM32F407VET6`
- 开发板：立创天空星 STM32F407VET6 开发板
- 系统：`RT-Thread 4.1.0`
- IDE：`RT-Thread Studio`
- 肌电采集：`ADS1292R`
- 姿态传感器：`MPU6050`
- 上位机：Python + Matplotlib GUI
- 预警执行器：1027 扁平震动马达，额定 `3.0V-3.7V`，最大电流约 `80mA`

重要硬件结论：

- 震动马达不能直接接 STM32 GPIO。
- GPIO 只能输出控制信号，不能直接带 80mA 马达。
- 马达需要用 `N 沟道 MOS 管` 或三极管驱动。
- 电池供电建议先用 `5V 充电宝 -> Type-C`，后续再做锂电池升压/稳压方案。

---

## 3. 当前板端程序状态

板端工程：

`D:\RT-ThreadStudio\workspace\test_pro3`

主要代码：

- `applications/main.c`
- `applications/emg_pipeline.c`
- `applications/emg_pipeline.h`
- `applications/ads1292.c`
- `applications/ads1292.h`

当前板端已经实现：

1. `ADS1292R` 初始化。
2. SPI 通信打通。
3. 能读出 `breath` 和 `ecg/肌电` 通道数据。
4. 上电后可以自动初始化 ADS1292R。
5. 能通过串口输出 `LIVE` 数据行：

   ```text
   LIVE,t_ms,ac,rms,mav,wl,zc,ssc,zcr_x1k,fatigue,alert,emg_phase,emg_status,mot_phase,angle_x10,active,ax,ay,az,gx,gy,gz
   ```

6. 能输出 `RAW` 原始肌电数据：

   ```text
   RAW,t_ms,emg
   RAW,123456,18432
   RAW,123458,17654
   ```

7. RAW 默认采样率目标是 `500Hz`。
8. 板端已有基于膝关节角度阈值的计数/动作阶段逻辑。
9. 板端已有 `fit_start` / `fit_stop` 命令控制 LIVE 输出。
10. 板端已有 `raw_start` / `raw_stop` / `raw_ch1` / `raw_ch2` 等命令用于 RAW 输出和通道切换。

重要限制：

- 当前疲劳模型和动作模型主要在电脑端跑，不是在 STM32 上跑。
- 板端虽然有一些基于阈值的疲劳/动作逻辑，但不是最终 Python 模型。
- 目前方案 D 的工作方式是：

  ```text
  STM32 采集数据 -> 串口发给电脑 -> Python 上位机跑模型 -> 显示结果
  ```

---

## 4. 上位机程序状态

主上位机：

`D:\mpu_wave\monitor.py`

主要功能：

1. 连接串口，例如 `COM12 @ 115200`。
2. 解析板端 `LIVE` 行。
3. 解析板端 `RAW` 行。
4. 实时显示：
   - EMG envelope / MAV
   - EMG raw waveform
   - Knee tilt angle
   - IMU magnitude
5. 支持按键打标签：

   ```text
   1 = Squat
   2 = Walking
   3 = Running
   4 = Deadlift
   5 = FreshSquat
   6 = FatiguedSquat
   0 = Rest
   ```

6. 保存日志：

   ```text
   D:\mpu_wave\session_YYYYMMDD_HHMMSS.log
   ```

7. 支持 `--infer`，即方案 D 实时推理：

   ```powershell
   python D:\mpu_wave\monitor.py COM12 --baud 115200 --fit-start --infer
   ```

8. 实时推理结果显示在 GUI 右侧 `PC INFER`。
9. 上位机会写入 `PRED` 行到日志，方便事后分析：

   ```text
   PRED,t_ms,action_label,raw_action,action_confidence,...
   ```

当前默认模型路径已经切换到：

```text
动作模型：
D:\mpu_wave\rt_thread_project\test_pro3\python\kneepad_action_rf.joblib

疲劳模型：
D:\mpu_wave\training_curated\fatigue_emg_balanced_sessions.joblib
```

---

## 5. 当前模型状态

当前模型说明文件：

`D:\mpu_wave\CURRENT_MODELS.md`

### 5.1 动作模型

当前默认动作模型：

```text
D:\mpu_wave\rt_thread_project\test_pro3\python\kneepad_action_rf.joblib
```

来源：

- 外部 `KneE-PAD` 数据训练。

当前类别：

```text
LegExt
Squat
Walking
```

非常重要：

- 这个模型没有 `Deadlift` 类。
- 因此现在只能粗略测试 `Squat` 和 `Walking`。
- 它不能正式识别硬拉。
- 硬拉需要后续自己采集 `Deadlift` 标签数据后重新训练。

动作模型之前在外部 KneE-PAD 数据上按受试者分组交叉验证大约：

```text
Accuracy: 80.1%
Macro-F1: 84.9%
```

但这只是外部数据上的结果，不代表一定适配我们的护膝。

当前动作模型实际限制：

- 我们自己的动作数据目前筛出来只有 `Squat` 类窗口。
- 还没有足够的 `Walking` 和 `Deadlift` 训练窗口。
- 所以目前不能训练出真正的 `Squat / Deadlift / Walking` 三分类模型。

用于未来训练目标三分类动作模型的脚本：

```text
D:\mpu_wave\train_action_squat_deadlift_walking.py
```

训练命令示例：

```powershell
python D:\mpu_wave\train_action_squat_deadlift_walking.py D:\mpu_wave\training_curated\action_windows.csv
```

但只有当 `action_windows.csv` 中同时有 `Squat / Deadlift / Walking` 三类数据时才有意义。

### 5.2 疲劳模型

当前默认疲劳模型：

```text
D:\mpu_wave\training_curated\fatigue_emg_balanced_sessions.joblib
```

类别：

```text
0 = Fresh
1 = Fatigued
```

这个模型是目前几版疲劳模型里“直接识别准确率最高”的一版。

在精选平衡数据上测试结果：

```text
Accuracy: 97.7%
F1-score: 97.6%
AUC: 99.8%

Fresh:    425 正确，8 错误
Fatigued: 382 正确，11 错误
```

重要警告：

- 这个高准确率是当前精选数据上的直接测试结果。
- 严格按 session 留出测试时，泛化效果明显下降。
- 所以它可以用于演示和初步识别，但还不能当作最终医疗/产品级可靠模型。

旧疲劳模型：

```text
D:\mpu_wave\fatigue_emg_only.joblib
```

这个已经不是默认首选，因为当前精选模型表现更高。

---

## 6. 数据状态

主要数据目录：

```text
D:\mpu_wave\training_curated
```

里面的关键文件：

```text
action_windows.csv
fatigue_windows.csv
fatigue_windows_balanced_sessions.csv
session_quality_report.csv
fatigue_emg_balanced_sessions.joblib
fatigue_emg_curated.joblib
```

### 6.1 动作数据

当前筛选结果：

```text
动作窗口：974 个
全部都是 Squat
Walking：0
Deadlift：0
```

结论：

- 当前动作数据只能作为 `Squat` 类保存。
- 不能训练三分类动作模型。
- 必须补采：
  - `Walking`
  - `Deadlift`

### 6.2 疲劳数据

当前筛选结果：

```text
总疲劳窗口：1077 个
FreshSquat：684
FatiguedSquat：393
```

其中平衡 session 数据：

```text
D:\mpu_wave\training_curated\fatigue_windows_balanced_sessions.csv
```

用于训练当前默认疲劳模型。

### 6.3 质量筛选逻辑

筛选脚本：

```text
D:\mpu_wave\curate_training_data.py
```

运行：

```powershell
python D:\mpu_wave\curate_training_data.py
```

输出：

```text
D:\mpu_wave\training_curated\action_windows.csv
D:\mpu_wave\training_curated\fatigue_windows.csv
D:\mpu_wave\training_curated\session_quality_report.csv
```

筛选原则：

- 动作训练需要 IMU 正常。
- 如果 IMU 全 0，这组数据不能用于动作训练。
- 疲劳模型主要用 EMG，所以 IMU 坏了的数据仍可用于疲劳训练，只要 Fresh/Fatigued 标签和 EMG 特征有效。
- `FreshSquat` 和 `FatiguedSquat` 同时可以：
  - 作为疲劳模型的 Fresh/Fatigued 标签。
  - 在动作模型中作为 `Squat` 动作样本。

---

## 7. 最近遇到的重要问题

### 7.1 MPU6050 数据突然全 0

现象：

上位机角度图为 0，IMU magnitude 图没有加速度。

日志中看到：

```text
LIVE,...,angle_x10,active,ax,ay,az,gx,gy,gz
LIVE,...,0,1,0,0,0,0,0,0
```

判断：

- 不是上位机图的问题。
- 是板端发来的 MPU6050 数据全 0。

正常情况下：

- 静止时加速度也不应该是 0。
- 至少某个轴应该接近 `±16384`，代表 1g 重力。

可能原因：

- MPU6050 接线松。
- I2C 没起来。
- 地址不对，默认 `0x68`，AD0 拉高时可能变 `0x69`。
- 板子上电时 MPU6050 初始化失败。

排查方法：

1. 打开上位机：

   ```powershell
   python D:\mpu_wave\monitor.py COM12 --baud 115200 --fit-start --infer
   ```

2. 不关闭上位机，直接按开发板复位键。
3. 看终端/日志开头是否有：

   ```text
   MPU6050 init failed
   MPU6050 thread start failed
   MPU6050 ready on i2c1, WHO_AM_I=0x68
   ```

4. 如果没有 ready，需要检查 MPU6050 供电、SDA、SCL、GND、地址。

### 7.2 串口速率和 RAW 数据

RAW 目标是 500Hz。

如果用 115200 输出 ASCII RAW，可能存在丢数据/间隔不稳定问题。

之前曾讨论过：

- RAW 数据适合疲劳特征提取。
- 动作识别需要 LIVE 中的 IMU 字段。
- 采动作数据时不要用 `--raw-only`，因为 `--raw-only` 会停掉 LIVE。

动作采集推荐：

```powershell
python D:\mpu_wave\monitor.py COM12 --baud 115200 --fit-start --infer
```

疲劳 RAW 采集时才考虑：

```powershell
python D:\mpu_wave\monitor.py COM12 --baud 115200 --raw-start
```

或更高波特率方案。

---

## 8. 震动马达接线方案

马达型号：

- 1027 扁平马达
- 红线正极
- 蓝线负极
- 额定电压 `3.0V-3.7V`
- 最大电流约 `80mA`

不能直接接 GPIO。

原因：

- STM32 GPIO 是控制信号，不是电源输出。
- GPIO 只能安全输出十几 mA 级别。
- 马达启动瞬间电流可能更大。
- 马达是感性负载，断电时会产生反向电压尖峰。
- 直接接 GPIO 可能烧 IO、导致板子复位或引入噪声。

推荐 MOS 管驱动：

需要器件：

```text
AO3400 / SI2302 / IRLML2502 N 沟道 MOS
1N5819 二极管
100Ω 电阻
100kΩ 电阻
100uF 电容
```

接线：

```text
马达红线 -> 3.3V 或 3.7V 马达电源正极
马达蓝线 -> MOS 管 D 漏极
MOS 管 S 源极 -> GND
MOS 管 G 栅极 -> STM32 GPIO，中间串 100Ω
MOS 管 G 栅极 -> 100kΩ -> GND

1N5819 二极管白线端/阴极 -> 马达红线
1N5819 二极管另一端/阳极 -> 马达蓝线

100uF 电容正极 -> 马达电源正极
100uF 电容负极 -> GND

STM32 GND 与马达电源 GND 必须共地
```

仅测试马达是否能震时，可以短时间：

```text
马达红线 -> 开发板 3.3V
马达蓝线 -> 开发板 GND
```

这样会一直震，不能程序控制。

再次强调：

```text
不要把马达红线接 GPIO、蓝线接 GND。
```

---

## 9. 电池供电建议

当前开发阶段最稳方案：

```text
5V USB 充电宝 -> Type-C -> 开发板
```

可穿戴方向方案：

```text
3.7V 锂电池 / 18650 / 聚合物锂电池
-> 充电保护模块
-> 5V 升压模块
-> 开发板 Type-C 或 5V/GND
```

建议：

- 容量：`2000mAh` 起步更稳。
- 升压模块：`5V/1A` 起步，最好 `5V/2A`。
- 马达最好不要直接从敏感模拟前端附近取电。
- 马达电源旁边加电容，减少对 ADS1292R 肌电信号的干扰。

不要这样做：

```text
不要把 3.7V 锂电池直接接开发板 5V
不要把 7.4V 电池直接接 5V
不要裸锂电池无保护使用
不要让马达和 ADS1292R 模拟前端共用很细很长的电源线
```

---

## 10. 当前推荐使用方式

### 10.1 实时演示

```powershell
python D:\mpu_wave\monitor.py COM12 --baud 115200 --fit-start --infer
```

GUI 右侧看：

```text
PC INFER
```

里面会显示动作和疲劳预测。

### 10.2 动作测试

当前能粗略测试：

```text
Squat
Walking
```

因为当前外部动作模型不含 Deadlift。

采集建议：

```text
按 0：Rest，静止 5 秒
按 1：Squat，做深蹲 20-30 秒
按 0：Rest
按 2：Walking，走路 20-30 秒
按 0：Rest
按 4：Deadlift，硬拉 20-30 秒
按 0：Rest
```

注意：

- 现在按 4 可以记录 Deadlift 标签。
- 但当前模型不会识别 Deadlift。
- 采到足够 Deadlift 后，才能训练新三分类模型。

### 10.3 疲劳测试

采集建议：

```text
按 5：FreshSquat
刚开始不累时做深蹲
按 0：Rest

持续运动到明显疲劳

按 6：FatiguedSquat
疲劳后再做深蹲
按 0：Rest
```

### 10.4 采完后筛选训练数据

```powershell
python D:\mpu_wave\curate_training_data.py
```

### 10.5 重新训练疲劳模型

全部精选疲劳数据：

```powershell
python D:\mpu_wave\train_fatigue_emg_only.py D:\mpu_wave\training_curated\fatigue_windows.csv --add-ratios --save-model D:\mpu_wave\training_curated\fatigue_emg_curated.joblib
```

平衡 Fresh/Fatigued session：

```powershell
python D:\mpu_wave\train_fatigue_emg_only.py D:\mpu_wave\training_curated\fatigue_windows_balanced_sessions.csv --add-ratios --save-model D:\mpu_wave\training_curated\fatigue_emg_balanced_sessions.joblib
```

### 10.6 训练动作三分类模型

前提：

`action_windows.csv` 中必须同时有：

```text
Squat
Deadlift
Walking
```

训练：

```powershell
python D:\mpu_wave\train_action_squat_deadlift_walking.py D:\mpu_wave\training_curated\action_windows.csv
```

训练后会生成：

```text
D:\mpu_wave\action_squat_deadlift_walking_rf.joblib
```

然后启动上位机时指定新模型：

```powershell
python D:\mpu_wave\monitor.py COM12 --baud 115200 --fit-start --infer --action-model D:\mpu_wave\action_squat_deadlift_walking_rf.joblib
```

---

## 11. 板端部署与模型体积问题

当前模型主要是随机森林。

之前估算：

- 动作模型：`400 棵树 x 深度 12`，体积非常大，约 MB 级。
- 疲劳模型：也偏大。
- STM32F407VET6 Flash 约 `1MB`，RAM `192KB`。

结论：

- 当前完整 sklearn `.joblib` 模型不能直接放板子上。
- 现在方案 D 是最实际的：

  ```text
  板子采集数据
  电脑跑完整模型
  电脑显示结果
  后续电脑可通过串口命令让板子震动
  ```

后续如果必须板端独立运行：

1. 需要精简模型。
2. 或改成更小的决策树/规则模型。
3. 或把随机森林导出成 C，并减少树数量/深度。
4. 或升级到更大 Flash/RAM 的 MCU。

---

## 12. 同学接手后的优先任务

### 第一优先级：补齐动作数据

现在动作数据只有 Squat。

需要补：

```text
Walking：至少 3 组，每组 20-30 秒
Deadlift：至少 3 组，每组 20-30 秒
Squat：可以继续补 2-3 组
```

每组一定要按键打标签：

```text
0 Rest
1 Squat
2 Walking
4 Deadlift
```

采完后跑：

```powershell
python D:\mpu_wave\curate_training_data.py
```

看：

```text
D:\mpu_wave\training_curated\action_windows.csv
```

确认三类都有。

### 第二优先级：继续采规范疲劳数据

每次最好一组里同时包含：

```text
FreshSquat
FatiguedSquat
Rest
```

不要只采 Fresh。

只采 Fresh 也有价值，但它会让跨 session 评估不稳定。

推荐每次：

```text
FreshSquat 20-30 秒
Rest
运动到疲劳
FatiguedSquat 20-30 秒
Rest
```

### 第三优先级：实现疲劳时震动

当前模型在电脑端跑，所以最简单实现：

```text
电脑 monitor.py 检测到 Fatigued
-> 通过串口发命令给 STM32
-> STM32 控制 GPIO
-> GPIO 控制 MOS 管
-> 马达震动
```

需要板端加一个 MSH 命令或串口命令，例如：

```text
motor_on 300
```

或：

```text
vibe 300
```

然后上位机检测疲劳概率超过阈值时发送。

---

## 13. 关键文件清单

### 上位机/训练脚本

```text
D:\mpu_wave\monitor.py
D:\mpu_wave\curate_training_data.py
D:\mpu_wave\validate_action_model.py
D:\mpu_wave\train_action_squat_deadlift_walking.py
D:\mpu_wave\train_fatigue_emg_only.py
D:\mpu_wave\test_dual_real_data.py
D:\mpu_wave\dual_predict.py
D:\mpu_wave\predict_fatigue.py
D:\mpu_wave\CURRENT_MODELS.md
```

### 当前模型

```text
D:\mpu_wave\rt_thread_project\test_pro3\python\kneepad_action_rf.joblib
D:\mpu_wave\training_curated\fatigue_emg_balanced_sessions.joblib
```

### 当前精选数据

```text
D:\mpu_wave\training_curated\action_windows.csv
D:\mpu_wave\training_curated\fatigue_windows.csv
D:\mpu_wave\training_curated\fatigue_windows_balanced_sessions.csv
D:\mpu_wave\training_curated\session_quality_report.csv
```

### RT-Thread 工程

```text
D:\RT-ThreadStudio\workspace\test_pro3
D:\mpu_wave\rt_thread_project\test_pro3
```

---

## 14. 最后结论

当前项目已经不是“代码跑不起来”的阶段。

当前状态是：

```text
ADS1292R 采集基本打通
MPU6050 能工作，但偶尔出现全 0，需要注意接线/初始化
上位机能实时显示和保存日志
上位机方案 D 能跑动作模型和疲劳模型
疲劳模型当前可用于演示识别
动作模型当前只能粗略支持 Squat / Walking
Deadlift 还没有模型，需要补采数据
马达需要 MOS 管驱动，不能直接接 GPIO
电池建议先用 5V 充电宝供电
```

同学接手时最重要的是：

1. 不要直接拿当前动作模型当 `Squat / Deadlift / Walking` 完整模型。
2. 先补采 `Walking` 和 `Deadlift` 数据。
3. 采集时确保 MPU6050 不为 0。
4. 采完后跑 `curate_training_data.py`。
5. 三类动作数据齐了再训练真正的动作模型。
6. 疲劳模型当前可以先用 `fatigue_emg_balanced_sessions.joblib` 做演示。
7. 马达必须通过 MOS 管驱动。
