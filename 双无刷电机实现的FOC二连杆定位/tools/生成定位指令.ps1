param(
    [string]$Target,
    [string]$Replies,
    [ValidateSet('Auto', 'Positive', 'Negative')]
    [string]$Branch = 'Auto',
    [ValidateRange(0.5, 10.0)]
    [double]$ShoulderSpeed = 5.0,
    [ValidateRange(0.5, 10.0)]
    [double]$ElbowSpeed = 5.0
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'joint_angles.ps1')
$calPath = Join-Path $PSScriptRoot 'two_link_zero.json'
$solverPath = Join-Path $PSScriptRoot '平面定位.ps1'
$culture = [Globalization.CultureInfo]::InvariantCulture

function Get-Checksum([byte[]]$Bytes) {
    $total = 0
    foreach ($item in $Bytes) { $total += $item }
    return [byte]($total -band 255)
}

function Read-ReplyText([string]$InputText, [hashtable]$Angles) {
    $pattern = '(?i)(?<![0-9A-F])3E[\s-]+92[\s-]+0[12][\s-]+08(?:[\s-]+[0-9A-F]{2}){10}(?![0-9A-F])'
    foreach ($match in [regex]::Matches($InputText, $pattern)) {
        [byte[]]$bytes = @($match.Value -split '[\s-]+' | ForEach-Object { [Convert]::ToByte($_, 16) })
        if ((Get-Checksum $bytes[0..3]) -ne $bytes[4] -or (Get-Checksum $bytes[5..12]) -ne $bytes[13]) {
            throw ('Bad checksum in ID {0} reply. Read that axis again.' -f $bytes[2])
        }
        $Angles[[int]$bytes[2]] = [BitConverter]::ToInt64($bytes, 5)
    }
}

function New-A4Hex([byte]$Id, [long]$TargetCounts, [double]$Speed) {
    [byte[]]$header = @(0x3E, 0xA4, $Id, 0x0C)
    $speedCounts = [uint32][math]::Round($Speed * 100)
    [byte[]]$payload = [BitConverter]::GetBytes($TargetCounts) + [BitConverter]::GetBytes($speedCounts)
    [byte[]]$frame = $header + (Get-Checksum $header) + $payload + (Get-Checksum $payload)
    return [BitConverter]::ToString($frame).Replace('-', ' ')
}

while ($true) {
    if (-not $Target) { $Target = Read-Host '输入目标 X,Y（毫米）。大臂轴心为原点，向右 +X，沿图中箭头向上 +Y。例如 100,100。输入 Q 退出' }
    if ($Target -eq 'Q') { return }
    try {
        $xy = @($Target.Trim() -split '[\s,\uFF0C]+')
        if ($xy.Count -ne 2) { throw '请输入两个坐标，例如 120,120。' }
        $x = [double]::Parse($xy[0], $culture)
        $y = [double]::Parse($xy[1], $culture)
        if ([double]::IsNaN($x) -or [double]::IsNaN($y) -or [double]::IsInfinity($x) -or [double]::IsInfinity($y)) {
            throw '坐标必须是有限数字。'
        }
        $radius = [math]::Sqrt($x * $x + $y * $y)
        if ($radius -gt 200.0000001) {
            throw ('目标离 D2 轴心 {0:F2} mm，超出 200 mm 理论可达范围。' -f $radius)
        }
        if ($radius -lt 1e-9) { throw '原点对应两臂完全折叠，请选择其他点。' }
        $solutions = @(
            (& $solverPath -Mode Solve -X $x -Y $y -Elbow Positive)
            (& $solverPath -Mode Solve -X $x -Y $y -Elbow Negative)
        )
        break
    } catch {
        if ($Replies) { throw }
        Write-Warning "$($_.Exception.Message) 请重新输入目标。"
        $Target = ''
    }
}

if (-not (Test-Path -LiteralPath $calPath)) { throw 'Calibration missing. Run the existing Calibrate mode first.' }
$cal = Get-Content -Raw -LiteralPath $calPath | ConvertFrom-Json
if ($cal.shoulderSign -notin -1,1 -or $cal.elbowSign -notin -1,1 -or
    $null -eq $cal.shoulderZero -or $null -eq $cal.elbowZero) { throw 'Calibration file is invalid.' }

$angles = @{}
if ($Replies) {
    Read-ReplyText $Replies $angles
    if ($angles.Count -ne 2) { throw 'Supply a complete 0x92 reply for each motor ID.' }
} else {
    Write-Host '第 2 步：在 XCOM 读取当前角度。ID 2 = 3E 92 02 00 D2；ID 1 = 3E 92 01 00 D1'
    while ($angles.Count -lt 2) {
        $line = Read-Host '粘贴一条或两条 0x92 回复（Q 退出）'
        if ($line -eq 'Q') { return }
        $countBefore = $angles.Count
        try { Read-ReplyText $line $angles }
        catch { Write-Warning $_.Exception.Message; continue }
        if ($angles.Count -eq $countBefore) { Write-Warning 'No new complete angle reply for the remaining ID.' }
    }
}

$shoulderOffset = Get-WrappedDeltaCounts $cal.shoulderZero $angles[2]
$elbowOffset = Get-WrappedDeltaCounts $cal.elbowZero $angles[1]
$currentShoulder = $cal.shoulderSign * $shoulderOffset / 100.0
$currentElbow = $cal.elbowSign * $elbowOffset / 100.0
$radians = [math]::PI / 180
$a = ($currentShoulder + $workspaceRotationDeg) * $radians
$b = $a + $currentElbow * $radians
Write-Host ('Current estimated XY: ({0:F2}, {1:F2}) mm.' -f
    (100 * ([math]::Cos($a) + [math]::Cos($b))), (100 * ([math]::Sin($a) + [math]::Sin($b))))
Write-Host ('Target XY: ({0:F2}, {1:F2}) mm.' -f $x, $y)
Write-Host ('Shoulder correction: {0:+0.00;-0.00;0.00} deg.' -f (-$workspaceRotationDeg))

$candidates = for ($i = 0; $i -lt $solutions.Count; $i++) {
    $solution = $solutions[$i]
    $label = if ($i -eq 0) { 'Positive' } else { 'Negative' }
    $shoulder = $solution.Shoulder + 360 * [math]::Round(($currentShoulder - $solution.Shoulder) / 360)
    $shoulderRaw = $angles[2] - $shoulderOffset + [long][math]::Round(100 * $cal.shoulderSign * $shoulder)
    $elbowRaw = $angles[1] - $elbowOffset + [long][math]::Round(100 * $cal.elbowSign * $solution.Elbow)
    $delta2 = $shoulderRaw - $angles[2]
    $delta1 = $elbowRaw - $angles[1]
    [pscustomobject]@{
        Branch = $label
        Shoulder = $shoulder
        Elbow = $solution.Elbow
        Raw2 = $shoulderRaw
        Raw1 = $elbowRaw
        Travel = ([math]::Abs($delta2) + [math]::Abs($delta1)) / 100.0
        ElbowTravel = [math]::Abs($delta1)
    }
}
foreach ($candidate in $candidates) {
    Write-Host ('{0}: shoulder {1:F2} deg, elbow {2:F2} deg, total travel {3:F2} deg.' -f
        $candidate.Branch, $candidate.Shoulder, $candidate.Elbow, $candidate.Travel)
}
$selected = if ($Branch -eq 'Auto') {
    $candidates | Sort-Object Travel, ElbowTravel | Select-Object -First 1
} else {
    $candidates | Where-Object Branch -eq $Branch
}
Write-Host ('Selected {0}. Raw final targets: ID 2 = {1:F2} deg; ID 1 = {2:F2} deg.' -f
    $selected.Branch, ($selected.Raw2 / 100.0), ($selected.Raw1 / 100.0))
Write-Host '第 3 步：在 XCOM 单条发送中逐条发送，取消定时发送。'
foreach ($id in 2,1) {
    $final = if ($id -eq 2) { $selected.Raw2 } else { $selected.Raw1 }
    $remaining = $final - $angles[$id]
    $joint = if ($id -eq 2) { '大臂' } else { '小臂' }
    $speed = if ($id -eq 2) { $ShoulderSpeed } else { $ElbowSpeed }
    Write-Host ''
    Write-Host ('{0} ID {1}，最终目标 {2:F2}°，需要转 {3:+0.00;-0.00;0.00}°，指令限速 {4:0.##}°/秒。' -f
        $joint, $id, ($final / 100.0), ($remaining / 100.0), $speed)
    New-A4Hex $id $final $speed
    if ($id -eq 2) {
        Write-Host '收到完整的 3E A4 02 应答后，即可清空发送框并发送下面的小臂指令。大臂可以仍在运动。'
    } else {
        Write-Host '确认收到 3E A4 01 应答。小臂停稳后，读回两轴角度核对是否到位。'
    }
}
