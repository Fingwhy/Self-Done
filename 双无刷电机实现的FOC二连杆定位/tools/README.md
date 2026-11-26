# 脚本使用

脚本来自本机已经用于实验的版本，原样保留运动参数。运行环境为 Windows PowerShell，调用 .NET SerialPort。COM10 为历史端口，按本机设备管理器修改。

| 文件 | 功能与入口 |
|---|---|
| 生成定位指令.ps1 | 输入 Target / Replies，打印两组逆解和选定 A4，默认限速各 5°/s。计算入口不打开串口 |
| 平面定位.ps1 | Math / Solve 只算数学；Plan 读串口生成分步计划；Run 发运动；Calibrate 进行标定操作 |
| 记录小臂.ps1 | 默认读串口基线；Move 发运动并记录；TraceOutput 增加状态和异常停止；SelfTest 离线验证 |
| joint_angles.ps1 | 角差绕圈与工作区 1.18° 配置 |
| two_link_zero.json | 本机原始零位和正方向，单位 0.01° |

默认只生成指令的交互入口为 `powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File ".\tools\生成定位指令.ps1"`。实际使用输入本机两轴最新回包，不能沿用文档静态读数直接运动。

离线验证为 `powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File ".\tools\记录小臂.ps1" -SelfTest`，它不打开串口。

`记录小臂.ps1 -Port COM10 -MotorId 1 -Move -TraceOutput` 会向 ID 1 发一次正向 2°、限速 3°/s 的 A4。脚本当前限定测试电压 19～21 V，负向实验增加 -Reverse，改变幅度使用 -Degrees，支持 0.5～30°。该检查属于已有诊断流程，任意供电范围的适用性需按 V1 核实。

`平面定位.ps1 -Mode Run` 按约 2° 交替单轴步进，读两轴位置并等待到位。现有检查包括单步最大 2.30°、另一轴移动 0.50°、走反 0.50°、过目标 1°、到位误差 0.20° 持续 250 ms 以及 5 s 单步超时。它与 XCOM 手动一次发最终 A4 的运动方式不同。

串口打开成功后生成 serial_logs。本仓库归档的历史日志位于 ../data/serial_logs。0x9C 的 Output 保存有符号原始值，该字段的安培解释尚未确认。工具使用当前原始角作为多圈目标计算依据，坐标数学可达也需检查实体路径。
