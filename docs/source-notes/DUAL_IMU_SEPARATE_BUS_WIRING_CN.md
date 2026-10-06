# 双 MPU6050 独立总线接线说明

当前固件使用两条独立的软件 I2C 总线，两颗 MPU6050 都使用默认地址 `0x68`。

| 连接 | 大腿 MPU6050 | 小腿 MPU6050 |
|---|---|---|
| VCC | 3V3 | 3V3 |
| GND | GND | GND |
| SCL | PB6 | PB8 |
| SDA | PB7 | PB9 |
| AD0 | GND | GND |
| INT | 不接 | 不接 |
| XDA/XCL | 不接 | 不接 |

3V3 与 GND 可以通过分线板或一拖二线共用；两组 SCL/SDA 不要互相连接。

```text
大腿：PB6/SCL + PB7/SDA  -> 软件 i2c1 -> 地址 0x68
小腿：PB8/SCL + PB9/SDA  -> 软件 i2c2 -> 地址 0x68
```

如果小腿 MPU6050 模块没有自带 I2C 上拉电阻，需要分别增加：

```text
PB8/SCL -- 4.7kΩ -- 3V3
PB9/SDA -- 4.7kΩ -- 3V3
```

常见 GY-521 模块通常已经带上拉电阻，首次接线时不要重复添加。建议在小腿模块的 VCC/GND 附近增加 `0.1uF + 4.7～10uF` 去耦电容。

开机后应看到：

```text
KIMU,READY,bus=i2c2,thigh=0x68,shank=0x68
```

如果出现 `KIMU,ERROR,shank_not_found`，依次检查 PB8、PB9、AD0=GND、3V3 和共地。
