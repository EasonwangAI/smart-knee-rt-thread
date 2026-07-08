# P03 疲劳训练数据说明

这个文件夹由 `D:\mpu_wave\P03.mat` 整理生成，用于肌电疲劳模型训练。

优先使用这个文件：

```text
features\features_lg0_binary_1s_hop0p5.csv
```

它只包含同一个 `LG_0` 工况下的二分类数据：

- `fatigue_label = 0`：`OriginalData/Ideal/LG_0`
- `fatigue_label = 1`：`OriginalData/Fatigue/LG_0`

如果要把所有可用工况都放进模型，可以使用：

```text
features\features_all_conditions_1s_hop0p5.csv
```

另有一份来自 `NormalizedData` 的周期级特征：

```text
features\features_normalized_cycles.csv
```

它适合做步态周期/左右侧分段级别的实验，但不建议作为第一版板端疲劳算法的主训练集。

但第一版模型建议先用 `LG_0`，因为它能减少“坡度/工况差异”对疲劳判断的干扰。

目录内容：

```text
features\                 训练特征 CSV，已计算 RMS/MAV/WL/ZC/SSC/MDF/MPF
raw_trials_npz\            每个 trial 的原始 13 通道 EMG，NumPy .npz 格式
metadata\trial_manifest.csv 每个 trial 的标签、工况、采样率和时长
train_fatigue_example.py   随手可跑的随机森林训练示例
```

快速训练示例：

```bash
cd D:\mpu_wave\fatigue_training_data
python train_fatigue_example.py
```

重要提醒：评估时必须按 `source_file` 或 trial 分组划分训练集和测试集，不要随机打散所有窗口，否则同一段试次的相邻窗口会同时出现在训练和测试里，准确率会虚高。
