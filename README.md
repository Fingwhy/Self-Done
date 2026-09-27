# 2.4 寸 LCD 副屏驱动板

一款基于 ESP32-S3 的 2.4 寸 LCD 副屏驱动板，用于桌面副屏场景：播放 TF 卡视频、显示 PC 监控信息（CPU/GPU 温度等）。

## 项目概览

| 项目 | 内容 |
|---|---|
| 主控 | ESP32-S3-WROOM-1-N4R2（4MB Quad Flash + 2MB Quad PSRAM） |
| 屏幕 | 2.4 寸 240×320 ST7789P3，8080 8-bit 并口，RGB565 |
| 存储 | MicroSD（TF）卡，SDMMC 4-bit |
| 电源 | VBUS(5V) → HR1117 LDO → 3.3V |
| USB | Type-C，支持 USB MSC（U 盘模式） |
| 开发环境 | Arduino IDE + ESP32 core 4.0.0 |

## 目标功能

- ✅ 60fps 视频播放（硬件架构支持，SDMMC 4-bit + 8080 并口）
- ✅ 板载 TF 卡
- ✅ PC 通过 USB 把视频导入 TF 卡（USB MSC U 盘模式）
- ✅ 软件退出后独立循环播放 TF 卡视频（待固件实现）
- ⏳ PC 软件实时显示 CPU/GPU 温度（待开发）
- ⏳ 电脑开机亮屏 / 关机熄屏（硬件已设计 Q2 互锁）

## 引脚映射

### LCD（8080 8-bit 并口）

| 信号 | GPIO |
|---|---|
| D0~D3 | IO4 / 5 / 6 / 7 |
| D4 | IO8 |
| D5~D7 | IO9 / 10 / 11 |
| WR | IO12 |
| DC(RS) | IO13 |
| RST | IO14 |
| CS | IO21 |
| TE（可选） | IO18 |
| 背光 BL_PWM | IO37 |

### TF 卡（SDMMC 4-bit）

| 信号 | GPIO |
|---|---|
| CLK | IO17（经 R8 22Ω） |
| CMD | IO15 |
| D0 | IO40 |
| D1 | IO41 |
| D2 | IO42 |
| D3 | IO16 |
| CD（卡检测） | IO38 |

### 其他

| 功能 | GPIO |
|---|---|
| USB D- / D+ | IO19 / IO20（原生 USB OTG） |
| UART0 TX / RX | IO43 / IO44（备选调试口 U4 排针） |
| BOOT（进下载） | IO0 |
| EN（复位） | EN |

## 背光电路说明

背光使用 **AO3401 P-MOS 高边开关**：

- S = 5V，D = 背光 LED 阳极链
- 栅极：R12(100Ω) ← IO37，R15(10kΩ) 上拉至 5V
- **低电平点亮**：IO37 = LOW → Vgs = −5V 全开
- 注意：IO37 拉高（3.3V）时 Vgs = −1.7V 仍部分导通（无法完全关断，V20 评审遗留 P1 缺陷，改版建议换低边 NMOS）

## 烧录配置（Arduino，务必照此）

- 开发板：**ESP32S3 Dev Module**
- ESP32 core：**4.0.0**（当前用 4.0.0-alpha1-cn）
- USB CDC On Boot：**Enabled**
- Flash Size：4MB，Flash Mode：QIO 80MHz
- PSRAM：**QSPI PSRAM**（N4R2 是 Quad PSRAM，选 OPI 会重启循环！）
- Upload Mode：UART0 / Hardware CDC
- 做 U 盘（MSC）时：USB Mode = **USB-OTG (TinyUSB)**

> 按键丝印是反的：**「RESET」键 = BOOT（IO0），「BOOT」键 = RESET（EN）**。
> 进下载模式：按住「RESET」键 → 点「BOOT」键 → 松开「RESET」键。

## 目录结构

```
Self-Done/
├── README.md                                # 项目说明（本文件）
├── firmware/                                # 固件代码
│   ├── LCD_Test/LCD_Test.ino                # LCD 颜色测试 + 背光 + U盘(MSC)（位击驱动）
│   ├── TF_Test/TF_Test.ino                  # TF 卡读取测试（列目录/读文件）
│   └── FPS_Test/FPS_Test.ino                # LCD 硬件 i80 8080 + DMA 帧率测试（点亮屏）
├── hardware/                                # 硬件工程（EasyEDA）
│   ├── LCD-V2.2.eprj2                       # V2.2 工程
│   ├── V2.2_backup/                         # V2.2 工程备份 (.epro2)
│   ├── LCD-V20.eprj2                        # V20 工程
│   ├── V20_backup/                          # V20/V21 工程备份 (.epro2/.eprj2)
│   └── 0819f05c4eef4c71ace90d822a990e87     # EasyEDA 自定义库文件
├── docs/                                    # 文档
│   └── 驱动板设计报告_V20检查与V21完善.html  # 设计检查与完善报告
└── 基于ESP32-S3-WROOM-1-N4R2的LCD屏幕/      # 早期上传（规格书/项目总结/工程）
```

## 调试进度

| 功能 | 状态 |
|---|---|
| 供电（5V / 3.3V） | ✅ 完成 |
| USB 枚举 + 烧录 | ✅ 完成 |
| 背光点亮 | ✅ 完成 |
| TF 卡读取（SDMMC 4-bit） | ✅ 完成 |
| U 盘模式（USB MSC） | ✅ 完成 |
| LCD 显示 | ✅ 完成（esp_lcd 硬件 i80 + DMA 点亮） |
| 视频播放 | ⏳ 待固件实现（读 TF 卡 + DMA 刷屏） |
| PC 软件（导视频/监控） | ⏳ 待开发 |

## 踩坑记录

1. **空片 USB 反复枚举**：空片会以 1 秒周期反复重启（ROM 内部定时器），手动进下载模式（IO0 低 + EN 复位）即稳定。
2. **C-to-C 供电失败**：CC1/CC2 需 5.1kΩ 下拉正确，否则 source 撤 VBUS。
3. **5V 走线间歇断裂**：VBUS 焊盘 5V 但 LDO VIN 掉 0.4~0.8V，靠重焊/飞线解决。
4. **PSRAM 模式配错导致重启循环**：N4R2 是 Quad PSRAM，必须选 QSPI PSRAM，选 OPI 会反复重启。
5. **背光极性反**：P-MOS 高边电路低电平点亮，与低边 NMOS 逻辑相反。
6. **第三方 USB MSC 库不兼容**：ESP32USBMSC 库是给 core 3.x 的；core 4.0 应直接用官方 `USBMSC.h`（在 `cores/esp32/`）。
7. **按键丝印反**：见上文「烧录配置」说明。
8. **位击驱动屏全黑 / 帧率低**：bit-bang 驱动 ST7789 时序不足、易全黑；改用 `esp_lcd` 硬件 LCD_CAM（i80 8080）+ DMA 后正常点亮，帧率可达硬件极限（参考 `FPS_Test`）。

## 下一步计划

1. 基于 `esp_lcd` 硬件 i80 + SDMMC 4-bit，实现 TF 卡视频解码 + DMA 播放（目标 60fps）
2. 开发 PC 端软件（USB 导入视频 + 监控数据显示 + 心跳/退出检测）
3. 验证「电脑开机亮屏 / 关机熄屏」互锁电路（Q2 + J1）
