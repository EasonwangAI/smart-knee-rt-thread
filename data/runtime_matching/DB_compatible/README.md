# DB_compatible

这个目录的 CSV 表头与 U 盘原有 `E:\DB` 目录一致，方便旧程序或旧说明继续识别。

注意：`reference_features.csv` 是从运行版匹配库按 person_id 聚合出来的兼容表。
它适合作为旧格式入口，但它只能表达“每个人一个参考特征”的旧结构。

真正做相似肌电片段检索时，仍建议使用上一层的：

- `runtime_units_normalized.csv`
- `runtime_matching_database.sqlite`
- `match_query.py`

因为旧 `reference_features.csv` 结构太简单，无法完整表达 1734 个窗口级匹配样本。
