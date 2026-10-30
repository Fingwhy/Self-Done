# firmware

固件待开发（ESP-IDF v6.1，target: esp32）。

## 计划模块

| 模块 | 内容 | 状态 |
|---|---|---|
| `can_rx` | TWAI 初始化（1Mbps，**NORMAL 模式**）、解析 `0x159` | 待开发 |
| `tmc2209` | UART 单线通信、写 `GCONF`/`MSTEP`/`IHOLD_IRUN` | 待开发 |
| `gripper` | 夹持状态机（张开 → 闭合并监测 StallGuard → 保持 → 松开） | 待开发 |
| `stallguard` | `SGTHRS` 阈值自适应标定 | 待开发 |
| `adc` | 母线电压采样（`VBUS_S`，GPIO35） | 待开发 |

## 关键注意

> **TWAI 必须用 `TWAI_MODE_NORMAL`，不能用 `LISTEN_ONLY`。**
> CAN 接收方要在 ACK 槽回显性位，只听模式不回 ACK，主控错误计数累积到 255 会 bus-off，机械臂直接掉线。

## 引脚分配（J4 → ESP32）

| J4 针 | 信号 | GPIO |
|---|---|---|
| 1 | GND | GND |
| 2 | STEP | 25 |
| 3 | DIR | 26 |
| 4 | EN | 27 |
| 5 | TMC_TX | 17 |
| 6 | UART_LINE | 16 |
| 7 | CAN_TX | 5 |
| 8 | CAN_RX | 4 |
| 9 | CAN_STB | 18 |
| 10 | VBUS_S | 35（ADC1） |
| 11 | DIAG | 34（ADC1/输入） |
