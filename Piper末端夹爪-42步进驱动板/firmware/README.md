# V21 固件

[J6 自动点动工程](V21_PiPER_J6_Auto_Jog/)使用 ESP-IDF 6.1，目标芯片为 ESP32。使用 VS Code 打开该工程，选中 ESP32 对应的串口后编译、烧录。烧录时断开 V21 到 ESP32 的 5 V 供电线。运行时拔掉开发板 USB，由 V21 J5.2 和 J5.4 为底座的绿色 EXT 5V 端子供电。

## 实测接线

| V21 | ESP32-WROOM-32E 开发板 |
|---|---|
| J4.1 GND | GND |
| J4.2 STEP | D25 |
| J4.3 DIR | D26 |
| J4.4 ENN | D27 |
| J4.5 TMC_TX | D32，ESP32 UART TX |
| J4.6 UART_LINE | D33，ESP32 UART RX |
| J4.8 CAN 收发器接收输出 | D22 |
| J5.2 5 V / J5.4 GND | 绿色 EXT 5V + / − |

ESP32 的 D21 CAN TX 保持断开。固件以 Classic CAN 1 Mbps 只听模式接入 PiPER J6，总线上不发送数据、ACK 或错误帧。TMC2209 通过 UART 配置，电机由 STEP/DIR 控制，ENN 为低有效。

## 实机测试

保持裸电机，在 PiPER 官方软件的夹爪控制页先提交 0 mm 建立基准，再提交 20 mm，最后改回 0 mm。以电机轴端为观察方向，预期依次顺时针、逆时针点动。每次最多输出 32 个脉冲，约 7.2° 电机轴指令转角。若希望裸轴每次转 18°，可把 main.c 中的 MAX_PULSES 改为 80 后重新烧录。目标值变化的方向用于选择转向，点动角度由脉冲数决定。

[四线联调记录](../docs/V21_J6四线联调_2026-11-25.md)
