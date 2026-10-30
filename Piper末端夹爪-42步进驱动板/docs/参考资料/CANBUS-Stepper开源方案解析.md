# CANBUS-Stepper v1.2 完整设计解析

> 来源：`github.com/omrfrkmll/CANBUS-Stepper`（默认分支 `master`，2 star，2026-07 创建）
> 解析时间：2026-10-29
> 硬件许可：**CC BY-NC-SA 4.0**（非商业 + 相同方式共享）
> 软件许可：MIT
> 开发状态：原理图 / PCB / BOM / STEP 已完成，**打样与固件仍在进行中**

---

## 0. 一句话概括

一块 40.3 × 40.3 mm 的板子，直接拧在 NEMA17 电机尾部的 31 mm 孔距上，把「ESP32-S3 + TMC2209 + AS5600 编码器 + CAN 收发器 + 完整保护」全塞进去，通过 CAN 总线接收位置/速度指令并回传状态。

它解决的是 3D 打印机工具头和多轴机器人的**末端驱动**问题：一根 4 芯线（24V + GND + CAN_H + CAN_L）替代原来十几根信号线。

---

## 1. 系统架构

```
        ┌─────────────── 24V / GND / CAN_H / CAN_L（4 芯线）───────────────┐
        │                                                                  │
   ┌────▼─────┐    ┌──────────────┐    ┌─────────────┐    ┌──────────────┐  │
   │ 保护链    │───▶│ LMR33630     │───▶│ TLV75533    │───▶│ ESP32-S3     │  │
   │ PPTC/PMOS│    │ 24V→5V 3A    │    │ 5V→3.3V     │    │ MINI-1-N4R2  │  │
   │ TVS      │    └──────────────┘    └─────────────┘    └───┬──────┬───┘  │
   │          │                                                │      │      │
   │          ├──────────────────────────────┐          TWAI  │      │ I2C  │
   │          │                              │            ┌───▼──┐ ┌─▼────┐ │
   │          ▼                              ▼            │TCAN  │ │AS5600│ │
   │    ┌──────────┐                   ┌──────────┐       │3413  │ │编码器│ │
   └───▶│ TMC2209  │◀── STEP/DIR/UART ─│ 运动控制  │       └───┬──┘ └──────┘ │
        │ -LA      │                   │ 1 kHz PID │           │            │
        └────┬─────┘                   └──────────┘           │            │
             │                                                 │            │
        ┌────▼─────┐                                           │            │
        │ 42 步进   │◀────────── AS5600 同轴磁编码器 ───────────┘            │
        └──────────┘                                                        │
                                    └────────────── CAN 总线 ───────────────┘
```

三个闭环：
1. **位置闭环** —— AS5600 读实际角度，与目标比较，1 kHz PID 修正步数
2. **电流环** —— TMC2209 内部斩波，软件通过 UART 改 `IRUN`
3. **热/压保护环** —— ADC 采母线电压和 NTC，过温欠压触发急停

---

## 2. 硬件规格全表

### 2.1 核心器件

| 功能 | 型号 | 关键参数 |
|---|---|---|
| MCU | **ESP32-S3-MINI-1-N4R2** | 双核 LX7 @240MHz，4MB Flash + 2MB PSRAM，WiFi4 + BLE5 |
| 步进驱动 | **TMC2209-LA** | VQFN-28，≤28V，2.0A RMS / 2.8A 峰值，UART |
| 编码器 | **AS5600-ASOT** | 12 bit / 4096 步/圈（0.088°），I2C 400kHz，地址 0x36 |
| CAN 收发器 | **TCAN3413DRBR** | 3.3V 逻辑，支持 CAN FD，±58V 总线故障保护 |
| 主降压 | **LMR33630CRNXR** | 3.8~36V 输入，3A 输出，2.1MHz 开关频率 |
| 3.3V LDO | **TLV75533PDRVR** | 500mA，超低压差 |
| 功率电感 | FNR4020S4R7MT | 4.7µH / 2A 屏蔽 |
| 输出电容 | 0603 22µF + 100µF/50V | 低 ESR 铝电解抑制电机斩波纹波 |

### 2.2 机械规格

| 项 | 值 |
|---|---|
| 板尺寸 | 40.3 × 40.3 mm，带切角 |
| 厚度 | 1.6 mm FR4，**2 层** |
| 安装孔 | M3 × 4，间距 **31.0 × 31.0 mm**（NEMA17 标准） |
| 编码器位置 | PCB 底面正中，与电机轴同轴 |

---

## 3. 电源架构

### 3.1 电源树

```
24V IN (Molex Micro-Fit 3.0)
   │
   ├─[PPTC 1.5A/24V]─[PMOS 反接 HSBB6115]─[TVS SMF26CA]─┬─── TMC2209 VM（直供）
   │                                                      │
   │                                                      └─── LMR33630 ── 5V/3A
   │                                                              │
   │                                                              ├── TLV75533 ── 3.3V/500mA ── ESP32-S3 + TCAN3413 + AS5600
   │                                                              └── （VBUS 分压检测点）
   │
   └── 24V OUT（Molex 直通，菊花链给下一块板）
```

### 3.2 每级的器件与理由

| 级 | 器件 | 参数 | 说明 |
|---|---|---|---|
| 过流 | `1210L150/24WR` | 1.5A 保持，24V | 自恢复，堵转时自动限流不用换保险 |
| 反接 | `HSBB6115` PMOS | −60V / −26A / 20mΩ | 压降只有 20mV@1A，比肖特基省 0.5W |
| 栅极钳位 | `BZT52C12S` | 12V 稳压管 SOD-323 | 防止 Vgs 超过 ±20V 打穿 |
| 瞬态 | `SMF26CA` | 26V / 200W 双向 TVS | 挡住电机制动回灌 |
| 主降压 | `LMR33630` | 3.8~36V → 5V / 3A | 24V 有 12V 余量，同步整流效率 >90% |
| LDO | `TLV75533` | 5V → 3.3V / 500mA | 低压差，给 MCU 干净电源 |

**为什么是两级（5V 再 3.3V）而不是 24V 直降 3.3V**：24V 直降 3.3V 的占空比只有 14%，DCDC 效率会掉到 70% 左右，而且纹波大；先用 LMR33630 降到 5V 再 LDO，效率高且 3.3V 干净。

### 3.3 电压监测

| 通道 | 引脚 | 分压 | 量程 |
|---|---|---|---|
| 母线电压 | GPIO2 (ADC1_CH1) | 100k / 10k = **11 倍** | 0~36.3V |
| USB 5V | GPIO7 (ADC1_CH6) | 同结构 | — |
| NTC 温度 | GPIO3 (ADC1_CH2) | 10k 上拉，100k NTC（B=4250） | — |

---

## 4. 保护链：五处

| 位置 | 器件 | 防什么 |
|---|---|---|
| 24V 入口 | PPTC 1.5A | 过流 / 短路 |
| 24V 入口 | PMOS + 12V 稳压管 | **反接**（最常见的手残事故） |
| 24V 入口 | SMF26CA TVS | 感性负载断开时的电压尖峰 |
| CAN 差分线 | PESD1CAN | ESD + 共模浪涌 |
| USB D+/D− | USBLC6-2P6 | ESD（超低电容，不破坏信号完整性） |
| USB VBUS | `JK-SMD0603-050/6V` PPTC 500mA | USB 侧过流 |
| USB / 主电 | MBR0520 肖特基 | **电源 OR-ing**：USB 和 24V 同时插时不倒灌 |

最后一条值得学：`MBR0520` 做 OR-ing，保证调试时插 USB 不会和 24V 打架。

---

## 5. 引脚全表（ESP32-S3）

### 5.1 TMC2209 接口

| 宏 | GPIO | 方向 | 说明 |
|---|---|---|---|
| `PIN_TMC_STEP` | 33 | 输出 | 步进脉冲 |
| `PIN_TMC_DIR` | 34 | 输出 | 方向 |
| `PIN_TMC_EN` | 48 | 输出 | **低有效**使能 |
| `PIN_TMC_TX` | 35 | 输出 | ESP32 TX → TMC RX |
| `PIN_TMC_RX` | 47 | 输入 | ESP32 RX ← TMC TX |
| `PIN_TMC_DIAG` | 21 | 输入 | StallGuard / 故障中断 |
| `PIN_TMC_INDEX` | 26 | 输入 | 微步索引输出 |

UART 参数：`Serial1`，**115200**，8N1，从机地址 `0b00`（MS1=MS2=GND）。
采样电阻 `R_SENSE = 0.11Ω`（原理图 R4、R6）。

### 5.2 编码器 / CAN / 模拟

| 宏 | GPIO | 说明 |
|---|---|---|
| `PIN_I2C_SDA` | 16 | AS5600 SDA |
| `PIN_I2C_SCL` | 14 | AS5600 SCL（400kHz） |
| `PIN_CAN_TX` | 38 | TWAI TX |
| `PIN_CAN_RX` | 37 | TWAI RX |
| `PIN_CAN_STB` | 39 | TCAN3413 待机，**低电平=正常工作** |
| `PIN_VBUS_SENSE` | 2 | 母线电压 ADC |
| `PIN_USB_SENSE` | 7 | USB 电压 ADC |
| `PIN_NTC_SENSE` | 3 | 温度 ADC |

### 5.3 人机与扩展

| 宏 | GPIO | 功能 |
|---|---|---|
| `PIN_LED1` | 1 | 心跳 / 活动指示 |
| `PIN_LED2` | 45 | CAN 活动 / 告警 |
| `PIN_SW1` | 6 | 设零点 |
| `PIN_SW2` | 36 | 急停 |
| `PIN_AUX1` | 4 | 扩展 / 限位 1 |
| `PIN_AUX2` | 5 | 扩展 / 限位 2 |

---

## 6. TMC2209 初始化序列

```cpp
// 上电默认状态
digitalWrite(PIN_TMC_EN, HIGH);   // 先禁用（低有效）
digitalWrite(PIN_TMC_STEP, LOW);
digitalWrite(PIN_TMC_DIR, LOW);

Serial1.begin(115200, SERIAL_8N1, PIN_TMC_RX, PIN_TMC_TX);  // 注意参数顺序 rx, tx
delay(20);

// 通信校验：读版本号，0x21 或 0x20 表示通
uint8_t version = driver.version();

// 配置
driver.toff(4);                  // toff=4 使能斩波（toff=0 时驱动关闭）
driver.blank_time(24);           // 消隐时间，滤掉开关尖峰
driver.rms_current(800);         // 默认 800mA RMS
driver.microsteps(16);           // 1/16 → 3200 步/圈
driver.pwm_autoscale(true);      // StealthChop 自动调压
driver.en_spreadCycle(false);    // 用 StealthChop（静音）
driver.TCOOLTHRS(0xFFFFF);       // CoolStep/StallGuard 速度阈值：全速域生效
driver.semin(5);                 // CoolStep 最小电流 5/32
driver.semax(2);                 // CoolStep 最大电流 2/32
driver.sedn(0b01);               // CoolStep 电流下降步长
driver.SGTHRS(100);              // StallGuard 灵敏度 0~255
```

**几个关键点**：

| 细节 | 为什么 |
|---|---|
| `toff(4)` 使能、`toff(0)` 禁用 | 这是关闭驱动最干净的方式，比只拉 EN 引脚更彻底 |
| `version() == 0x21` | TMC2209 的版本号固定是 0x21，可用作 UART 连通性自检 |
| `SGTHRS(100)` + `TCOOLTHRS(0xFFFFF)` | 让 StallGuard 在全部速度范围生效，`DIAG` 引脚拉高表示堵转 |
| `rms_current(800)` | 是 **RMS**，对应峰值 1.13A |

### 6.1 电流换算注意

TMCStepper 库的 `rms_current()` 传入的是 **RMS 值**，不是峰值。

| 电机标称 | 峰值 | 应填 RMS |
|---|---|---|
| 42CM04 | 1.5A | 1.06A → `rms_current(1060)` |
| 项目默认 | 1.13A | 800mA（偏保守） |

### 6.2 UART 是单线的

TMC2209 芯片只有一个 `PDN_UART` 引脚，但代码里用了两个 GPIO（35 发、47 收）。硬件上必须这样接：

```
ESP32 GPIO35 (TX) ──[1kΩ]──┬── TMC2209 PDN_UART
                            │
ESP32 GPIO47 (RX) ──────────┘
```

TX 串 1kΩ 做"软驱动"避免推挽冲突，RX 直连保证读数完整。

---

## 7. AS5600 编码器

| 项 | 值 |
|---|---|
| 分辨率 | 4096 计数/圈，0.0879°/count |
| 接口 | I2C 400kHz，地址 0x36 |
| 安装 | PCB 底面正中，与电机轴同轴，轴上贴**径向充磁**磁铁 |
| 状态量 | `magnet_detected`、`gain_agc`、`magnitude` |

固件用 `getMultiTurnAngle()` 做多圈累计——单圈只有 ±180°，夹爪只需要小角度，但多圈计数能防丢步累积。

`DEG_PER_COUNT = 360.0 / 4096.0`。

---

## 8. CAN 接口与协议

### 8.1 物理层

| 项 | 值 |
|---|---|
| 收发器 | TCAN3413，3.3V 逻辑，CAN FD 能力 |
| 待机控制 | `PIN_CAN_STB` 拉低才能通信（默认高阻=待机） |
| 默认波特率 | 500 kbps（代码支持 250k / 500k / **1M**） |
| 默认节点 ID | `0x120` |
| 终端电阻 | **拨码开关** `MSK12C01G15-W` 可选接入 120Ω |
| 连接器 | 2× JST SH 2P（输入 + 直通输出，可菊花链） |

### 8.2 编址方式（重点）

这个项目的 ID 设计很聪明，值得学：

```c
uint32_t cmd_type  = msg.identifier & 0x0F;    // 低 4 位 = 命令码
uint32_t target_id = msg.identifier & ~0x0F;   // 高位 = 节点 ID
if (target_id != _node_id && target_id != 0x00) return;  // 过滤
```

所以一个节点 ID 占用高 28 位，低 4 位留给 16 种命令。默认 `node_id = 0x120`。

### 8.3 命令表

| 方向 | ID（相对 node_id） | 命令 | 载荷 |
|---|---|---|---|
| 上位机→节点 | `\| 0x01` | 设目标位置 | float32 角度（4B） |
| | `\| 0x02` | 设目标速度 | float32 度/秒 |
| | `\| 0x03` | 设电流 | uint16 mA |
| | `\| 0x04` | 使能 | uint8：1=使能，2=急停，0=禁用 |
| | `\| 0x05` | 设零点 | 无 |
| | `\| 0x06` | 设 PID | 3×uint16（kp/100, ki/1000, kd/100） |
| | `\| 0x10` | 请求遥测 | 无 |
| 节点→上位机 | `\| 0x20` | 位置遥测 | float32 角度 + float32 速度 |
| | `\| 0x21` | 状态遥测 | uint16 mV + int16 温度×10 + uint32 标志 |

广播：`target_id = 0x00` 时所有节点都接收。

### 8.4 状态标志位

| bit | 含义 |
|---|---|
| 0 | StallGuard 堵转 |
| 1 | 电机使能中 |
| 2 | 过温 |
| 3 | 欠压 |

遥测周期 **20ms（50 Hz）**，由 `loop()` 里的时间判断触发。

---

## 9. 固件架构

### 9.1 双核任务分配

| 位置 | 内容 | 频率 |
|---|---|---|
| **Core 1** | `motionControlTask` —— 编码器读取 + PID + 发脉冲 | **1 kHz**（`vTaskDelayUntil` 精确 1ms） |
| Core 1 | `loop()` —— CAN 收发、ADC 采样、按键、串口 CLI、LED | 5ms 延时 |

运动任务用 `configMAX_PRIORITIES - 1`（最高优先级）钉在 Core 1，保证 1ms 周期不被打断。

### 9.2 上电初始化顺序（8 步）

1. USB CDC 串口 115200（`ARDUINO_USB_CDC_ON_BOOT=1`）
2. 用户 GPIO：LED、按键、AUX（上拉）
3. ADC 监控：读母线电压和板温
4. AS5600 编码器（失败只警告不中断）
5. TMC2209（**失败降级为 STEP/DIR 独立模式继续跑**）
6. 运动控制器
7. TWAI CAN（500k，node 0x120）
8. 创建 Core 1 运动任务

**第 5 步的降级设计值得学**：UART 不通时不让整机挂掉，退化成纯脉冲驱动，只是失去电流/细分可配置能力。

### 9.3 安全联锁

| 触发 | 动作 |
|---|---|
| NTC > 90°C | `emergencyStop()` + LED2 常亮 + 串口告警 |
| 母线欠压 | 状态位置 bit3（代码里有 `isUndervoltage()` 判断） |
| SW2 按下 | 急停 |
| SW1 按下 | 设当前为零点 |
| CAN 使能命令 = 2 | 急停 |

### 9.4 串口 CLI

调试用，支持：`P <角度>`、`V <速度>`、`EN`、`DIS`、`ZERO`、`CUR <mA>`、`PID <kp> <ki> <kd>`、`STATUS`。

`STATUS` 会打印编码器角度、目标/误差、速度、母线电压、温度、CAN 收发计数、磁铁检测状态——调试夹爪时非常有用。

### 9.5 依赖库

```ini
platform  = espressif32 @ ^6.6.0
board     = esp32-s3-devkitc-1
framework = arduino
lib_deps  =
    teemuatlut/TMCStepper @ ^0.7.3
    Wire
    bblanchon/ArduinoJson @ ^7.0.4
```

三套编译环境：`standalone_can`（独立 CAN 执行器）、`diagnostics`（硬件自检）、`microros`（ROS2 节点）。

---

## 10. 代码审查：三个要注意的地方

### 10.1 遥测 ID 与命令 ID 冲突

`CAN_RESP_TELEMETRY_POS = 0x20`，而 `0x20 & 0x0F = 0`，所以遥测帧 1 的 ID 算出来是 `0x120 | 0x20 = 0x120`——和节点基址相同。

遥测帧 2 是 `0x120 | 0x21 = 0x121`，而 `0x121` 正好是"设目标位置"命令的 ID。

**问题**：如果总线上另一块板发 0x121，本板会把它当成"设位置"命令解析前 4 字节当 float 用。

**修改建议**：响应码放到高位区，比如 `node_id | 0x100`、`node_id | 0x200`，或者干脆用独立的 ID 段。

### 10.2 float 直接 memcpy 没管字节序

```c
memcpy(&target_pos, msg.data, sizeof(float));   // 假设小端
```

ESP32 是小端，如果上位机是 x86（也是小端）没问题；但如果哪天用大端平台或某些 PLC，数据会完全错乱。建议协议里显式定义小端并用 `__builtin_bswap32` 兜底，或者干脆用定点数（×1000 的 int32）。

### 10.3 `isStalled()` 读 DIAG 引脚，但没做去抖

```cpp
bool TMC2209Handler::isStalled() {
    return digitalRead(PIN_TMC_DIAG) == HIGH;
}
```

StallGuard 的 DIAG 输出在临界状态会抖动，单次读取容易误判。建议连续读 3 次、间隔 2ms 都高才算堵转。

---

## 11. 移植到 ESP32-WROOM-32E 的映射建议

这个项目是 **ESP32-S3**，你手上是 **ESP32-WROOM-32E**。TWAI 外设完全相同，但引脚不能照搬（S3 的 GPIO 编号到 48，ESP32 只到 39，且 34-39 只能输入）。

| 功能 | 原（S3） | 建议（WROOM-32E） | 说明 |
|---|---|---|---|
| STEP | 33 | 25 | 可用 LEDC/MCPWM |
| DIR | 34 | 26 | |
| EN | 48 | 27 | |
| TMC UART TX | 35 | 17 | |
| TMC UART RX | 47 | 16 | |
| TMC DIAG | 21 | 34 | **只能输入**，正合适 |
| I2C SDA | 16 | 21 | |
| I2C SCL | 14 | 22 | |
| CAN TX | 38 | 5 | **不能用 34-39**（只输入） |
| CAN RX | 37 | 4 | |
| CAN STB | 39 | 18 | |
| VBUS ADC | 2 | 35 | ADC1 |
| NTC ADC | 3 | 32 | ADC1 |

**必须避开**：GPIO6~11（Flash）、GPIO0/2/12/15（strapping）、GPIO1/3（UART0）。

**注意**：ESP32 的 ADC2（GPIO0-15 部分）与 WiFi 冲突，所有 ADC 都要放 ADC1（GPIO32-39）。

---

## 12. 适配 Piper 的 `0x159` 协议

原项目的协议（`node_id | cmd`）和 Piper 的夹爪协议完全不同。如果要做成 Piper 兼容夹爪，需要在 `processIncomingFrame()` 里加一路分支：

| Piper 帧 | ID | 内容 |
|---|---|---|
| 接收 | `0x159` | Byte0-3 行程 int32 ×0.001mm；Byte4-5 扭矩 int16 ×0.001N·m；Byte6 bit0 使能 / bit1 清错；Byte7 `0xAE` 设零 |
| 回复 | `0x2A8` | 由主控发送，**不是夹爪发** |

要做的事：
1. 把 1.5A 峰值换算成 `rms_current(1060)`
2. 行程 mm → 齿轮齿条位移 → 步数（你的齿轮 m1.5 z14，分度圆 Φ21，`1 rad = 10.5mm`）
3. CAN 改成 **1 Mbps**（`TWAI_TIMING_CONFIG_1MBITS()`）
4. **TWAI 必须是 `TWAI_MODE_NORMAL`**（原项目已经是，正确）——不能用 LISTEN_ONLY，否则不回 ACK 会把主控打到 bus-off
5. 夹持力靠弹性夹指或 StallGuard，不能靠电流（步进恒流，电流不随负载变）

---

## 13. 许可证与合规

| 内容 | 许可 | 你能做什么 |
|---|---|---|
| 原理图、引脚、机械 STEP | **CC BY-NC-SA 4.0** | 个人学习/研究可用；**不能商用**；修改后分发必须以相同许可开源；必须署名 |
| 固件代码 | MIT | 可自由使用、修改、商用，保留版权声明即可 |

**如果将来要卖这个夹爪**，硬件部分不能直接抄这个设计，必须自己重新画；软件部分（MIT）可以自由用。

---

## 14. 参考链接

| 内容 | 地址 |
|---|---|
| 仓库主页 | `https://github.com/omrfrkmll/CANBUS-Stepper` |
| 原理图 PDF v1.2 | `docs/schematics/CANBUS_Stepper_v1.2_Schematic.pdf` |
| 3D STEP | `step/canbus-stepper-v1.2.step` |
| 五页原理图分工 | Sheet1 MCU+USB+电源OR-ing / Sheet2 TMC2209 / Sheet3 AS5600+传感器 / Sheet4 CAN / Sheet5 DCDC+保护 |

---

## 15. 可以借的 / 不能借的

| 能借（结构、思路） | 不能借（必须自己改） |
|---|---|
| 电源树的"PPTC→PMOS→TVS→DCDC→LDO"顺序 | 整板尺寸（你是末端夹爪，形状不同） |
| 五处保护的位置和器件选型 | MCU 型号（S3 vs WROOM） |
| TELEMETRY 帧的状态位设计 | CAN ID 编址（要改成 Piper 的 0x159） |
| 核心任务的 1kHz 定时方式 | 电流值（要按你的电机换算） |
| 驱动 UART 失败时的降级策略 | 编码器安装方式（取决于你的机械结构） |
| 串口 CLI 的命令集 | — |
