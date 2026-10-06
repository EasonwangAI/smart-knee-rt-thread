# STM32、ESP32 与手机的通信

## 数据链路

STM32 的 UART1 输出状态，ESP32 GPIO18 接收。GPIO17 可反向发送数据流启用命令。两端逻辑电平为 3.3 V，共地，波特率 115200，8N1。

## KNEE 帧

当前板端格式：

```text
KNEE,score,action,count,alert,quality,label,fail_mask,contact,walk,squat,deadlift
```

| 字段 | 含义 |
| --- | --- |
| score | 对外输出的疲劳分数，可能包含数据库融合和测试用门控 |
| action | SQUAT、DEADLIFT、WALK 或 UNKNOWN |
| count | 已结束训练动作总次数，当前含 UNKNOWN，不含步行 |
| alert | 对外报警状态 |
| quality / label | 动作质量分数及 GOOD/OK/WEAK/WRONG/WAIT 标签 |
| fail_mask | 动作质量未满足项的位掩码 |
| contact | 电极状态，OK/LOW/SAT/NOISY/WAIT 等 |
| walk / squat / deadlift | 分项累计次数 |

发送线程间隔为 500 ms。TFT 的 80 ms 刷新间隔不等于无线刷新间隔。

## PRESS 与 KIMU

```text
PRESS,med=...,lat=...,diff=...,status=BAL/MED/LAT/CAL/ERR/OFF,...
```

`med`、`lat` 等仍来自压力采样；当前 `status` 会由双 MPU 状态覆盖：`LAT` 对应内扣，`MED` 对应外扩，`BAL` 对应中性。它不是直接由压力差得到的姿态结论。`KIMU` 是双 MPU 的独立调试流。

ESP32 v1.3.2 解析 KNEE 和 PRESS，并通过 HTTP JSON 返回手机。数据超过约 3 秒未更新时，会尝试重新向 STM32 发送：

```text
knee_start
pressure_stream on
```

## HTTP 接口

| 方法 | 路径 | 功能 |
| --- | --- | --- |
| GET | `/` | 浏览器监测页面 |
| GET | `/api` | 分数、动作、次数、质量、接触、姿态和数据延迟 |
| POST | `/api/pair/verify` | 配对码校验 |
| GET | `/api/profile` | 用户档案状态 |
| POST | `/api/profile/select` | 选择档案 |
| POST | `/api/profile/save` | 保存档案 |
| POST | `/api/profile/delete` | 删除档案 |
| POST | `/api/training/start` | 开始训练 |
| POST | `/api/training/end` | 结束训练 |

POST 使用表单参数，受保护操作要求 `pair_code`。固件默认热点密码和配对码是开发演示默认值，已公开在源码中。

ESP32 按个人档案产生的建议阈值是无线/手机层逻辑，不会自动改写 STM32 的硬件报警阈值。
