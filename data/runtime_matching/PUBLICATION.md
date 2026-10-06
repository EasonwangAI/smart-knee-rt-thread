# 运行匹配库公开副本

来自本次工程的 `storage_payload/smart_knee_runtime_db`，保留 CSV、SQLite、标准化参数及板端兼容表。

原库共有 1,734 条匹配单元、27 个人员/外部受试者类型，其中自采单元 870 条，外部单元 864 条。此数量是特征片段数量，不是独立受试者数量或有标签疲劳测试数量。

公开副本将直接出现的受试者姓名按一对一映射替换为 `subject_XX`，保留 `local_XX` / `ext_mat_XX` 等已有代号、特征数值与匹配关系，并去掉机器绝对路径。每个人仍有独立代号，未合并人员类型。原始本地文件没有修改，姓名替换后没有重新训练模型。

`runtime_matching_database.sqlite` 供电脑端查询使用。STM32 固件读取 CSV 和内置参考特征，不在板端运行 SQLite。

可运行：

```bash
python match_query.py query_example.json device_core 5
python match_query.py query_example.json mixed_shape 5
```

`device_core` 优先使用本机自采特征，`mixed_shape` 允许外部特征参与形状相似度比较。外部样本没有可靠疲劳标签，不应把外部相似样本直接当作已知疲劳真值。

此公开库未包含完整原始 MAT/日志波形，也不包含姓名对照表。数据来源和使用授权应由原采集者/数据提供者继续维护。
