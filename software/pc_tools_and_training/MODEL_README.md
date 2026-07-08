# 肌肉疲劳检测模型 - 使用文档

## 📦 模型信息

### 最终模型
- **文件**: `fatigue_emg_only.joblib`
- **版本**: v1.0 (2个session训练)
- **性能**: 83.1% 准确率, 82.0% F1-score, 87.2% ROC AUC
- **训练数据**: 519个窗口 (278 Fresh + 241 Fatigued)
- **特征**: 12个 (7个基础EMG + 5个派生)

---

## 🎯 模型特点

### ✅ 优势
- **纯EMG特征**: 不依赖IMU数据，适合设备放置在桌面测试
- **实时计算**: 所有特征都可以从固件的LIVE输出直接获取
- **平衡性能**: Fresh和Fatigued识别率都在82-83%
- **低延迟**: 基于3秒滑动窗口，1秒更新一次

### 📊 性能指标
```
准确率:  83.1%
F1分数:  82.0%
ROC AUC: 87.2%

混淆矩阵:
              预测Fresh  预测Fatigued
真实Fresh         58         12       (82.9%)
真实Fatigued      10         50       (83.3%)
```

---

## 🔧 使用方法

### 1. Python 预测

```python
from joblib import load
import numpy as np

# 加载模型
model = load('fatigue_emg_only.joblib')

# 从固件LIVE输出获取7个基础特征
mav = 2500    # Mean Absolute Value
rms = 2000    # Root Mean Square
wl = 400000   # Waveform Length
iemg = 1200000  # Integrated EMG
var = 3000000   # Variance
zc = 5        # Zero Crossings
ssc = 3       # Slope Sign Changes

# 计算5个派生特征
zc_per_mav = zc / (mav + 1e-9)
ssc_per_rms = ssc / (rms + 1e-9)
rms_mav_ratio = rms / (mav + 1e-9)
wl_per_iemg = wl / (iemg + 1e-9)
var_per_rms2 = var / ((rms ** 2) + 1e-9)

# 组合所有12个特征
features = np.array([[
    mav, rms, wl, iemg, var, zc, ssc,
    zc_per_mav, ssc_per_rms, rms_mav_ratio,
    wl_per_iemg, var_per_rms2
]])

# 预测
prediction = model.predict(features)[0]  # 0=Fresh, 1=Fatigued
probability = model.predict_proba(features)[0, 1]  # 疲劳概率

print(f"状态: {'疲劳' if prediction == 1 else '新鲜'}")
print(f"疲劳概率: {probability:.1%}")
```

### 2. 使用预测脚本

```bash
cd D:\mpu_wave
python predict_fatigue.py
```

---

## 📈 特征重要性

| 排名 | 特征 | 重要性 | 说明 |
|------|------|--------|------|
| 1 | **RMS** | 12.8% | 均方根，反映肌肉募集强度 |
| 2 | **WL/IEMG** | 12.4% | 波形复杂度比率 |
| 3 | **WL** | 11.8% | 波形长度，反映信号复杂度 |
| 4 | **VAR** | 11.8% | 方差，反映信号稳定性 |
| 5 | **IEMG** | 10.3% | 积分肌电，反映总能量 |
| 6 | **MAV** | 10.3% | 平均振幅 |
| 7 | **RMS/MAV** | 9.6% | 振幅比率 |
| 8 | **VAR/RMS²** | 8.1% | 归一化方差 |

---

## 🔬 疲劳的EMG特征变化

### 典型变化模式

| 特征 | 新鲜状态 | 疲劳状态 | 变化 |
|------|---------|---------|------|
| **RMS** | 2000-2500 | 3000-3500 | ↑ 增加 |
| **MAV** | 2000-2500 | 3000-3500 | ↑ 增加 |
| **WL** | 350k-450k | 500k-600k | ↑ 增加 |
| **IEMG** | 1.0M-1.5M | 1.5M-2.0M | ↑ 增加 |
| **VAR** | 2M-4M | 5M-7M | ↑ 增加 |
| **ZC** | 4-6 | 2-4 | ↓ 降低 |
| **SSC** | 3-5 | 1-3 | ↓ 降低 |

### 生理原理
- **振幅增加** (RMS/MAV/WL ↑): 疲劳时需要募集更多运动单元
- **频率降低** (ZC/SSC ↓): 肌肉纤维传导速度下降
- **方差增加** (VAR ↑): 信号不稳定性增加

---

## 🚀 部署到嵌入式设备

### 固件已有的特征
你的 `emg_pipeline.c` 已经计算了7个基础特征：
```c
emg_snapshot_t snapshot;
emg_pipeline_get_snapshot(&snapshot);

// 基础特征 (已有)
uint32_t mav = snapshot.mav;
uint32_t rms = snapshot.rms;
uint32_t wl = snapshot.wl;
uint16_t zc = snapshot.zc;
uint16_t ssc = snapshot.ssc;
```

### 需要添加的计算
```c
// 计算IEMG (积分肌电)
uint64_t iemg = (uint64_t)mav * EMG_WINDOW_SAMPLES;

// 计算VAR (方差)
uint32_t var = rms * rms - mav * mav;

// 计算5个派生特征
float zc_per_mav = (float)zc / (mav + 1e-9f);
float ssc_per_rms = (float)ssc / (rms + 1e-9f);
float rms_mav_ratio = (float)rms / (mav + 1e-9f);
float wl_per_iemg = (float)wl / (iemg + 1e-9f);
float var_per_rms2 = (float)var / ((float)rms * rms + 1e-9f);
```

### 模型转换
可以使用以下工具将sklearn模型转换为C代码：
- **m2cgen**: 自动生成C代码
- **emlearn**: 嵌入式机器学习库
- **手动实现**: RandomForest可以转换为if-else决策树

---

## 📊 数据采集建议

### 采集协议
1. **新鲜状态** (按键5): 做10-15次标准深蹲
2. **休息** (按键0): 30-60秒
3. **疲劳状态** (按键6): 持续深蹲直到明显疲劳 (30-50次)
4. **重复**: 3-5轮以获得足够数据

### 质量要求
- 每个状态至少50个窗口 (~2-3分钟)
- 类别平衡: Fresh:Fatigued ≈ 1:1
- 多次采集: 至少2-3个独立session

---

## 📁 文件清单

```
D:\mpu_wave\
├── fatigue_emg_only.joblib          # 训练好的模型 ✅
├── fatigue_emg_features.csv         # 训练数据集
├── train_fatigue_emg_only.py        # 训练脚本
├── predict_fatigue.py               # 预测示例脚本
├── monitor.py                       # 数据采集上位机
├── auto_label.py                    # 特征提取脚本
└── quick_train.bat                  # 一键训练脚本
```

---

## ⚠️ 注意事项

1. **特征单位**: 确保输入特征的单位与训练数据一致
2. **归一化**: 模型未使用归一化，直接使用原始特征值
3. **窗口大小**: 基于3秒窗口 (256样本@500Hz)，1秒滑动
4. **IMU数据**: 当前模型不使用IMU，未来绑腿时可重新训练

---

## 🔄 模型更新

### 何时需要重新训练
- 更换电极位置
- 更换测试对象
- 采集到更多高质量数据
- 性能不满足要求

### 重新训练命令
```bash
cd D:\mpu_wave
python train_fatigue_emg_only.py session_*_features.csv --save-model fatigue_emg_only.joblib --add-ratios
```

---

## 📞 技术支持

如需帮助，请检查：
1. 模型文件是否存在: `fatigue_emg_only.joblib`
2. Python依赖是否安装: `sklearn`, `numpy`, `joblib`
3. 特征值是否在合理范围内

---

**模型版本**: v1.0  
**训练日期**: 2026-05-31  
**训练数据**: session_20260531_093520 + session_20260531_095213  
**性能**: 83.1% 准确率, 82.0% F1-score
