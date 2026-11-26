param(
    [ValidateSet('Math', 'Solve', 'Calibrate', 'Plan', 'Run')]
    [string]$Mode = 'Math',
    [string]$Port = 'COM10',
    [double]$X = 120,
    [double]$Y = 120,
    [ValidateSet('Positive', 'Negative')]
    [string]$Elbow = 'Positive'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'joint_angles.ps1')
$armLength = 100.0
$configPath = Join-Path $PSScriptRoot 'two_link_zero.json'
$logs = Join-Path $PSScriptRoot 'serial_logs'
$shoulderId = 2
$elbowId = 1

function Solve-Point {
    $c = ($X * $X + $Y * $Y - 2 * $armLength * $armLength) / (2 * $armLength * $armLength)
    if ($c -lt -1.000000001 -or $c -gt 1.000000001) { throw 'Target is outside the 200 mm reach.' }
    if ([math]::Abs($X) + [math]::Abs($Y) -lt 1e-9) { throw 'The folded origin has no unique shoulder angle.' }
    $c = [math]::Min(1.0, [math]::Max(-1.0, [double]$c))
    $q2 = [math]::Acos($c)
    if ($Elbow -eq 'Negative') { $q2 = -$q2 }
    $q1 = [math]::Atan2($Y, $X) - [math]::Atan2($armLength * [math]::Sin($q2), $armLength * (1 + [math]::Cos($q2)))
    [double[]]$degrees = @(
        ($q1 * 180 / [math]::PI - $workspaceRotationDeg)
        ($q2 * 180 / [math]::PI)
    )
    return ,$degrees
}

function Get-Checksum([byte[]]$Bytes) {
    $total = 0
    foreach ($item in $Bytes) { $total += $item }
    return [byte]($total -band 255)
}

function Write-Log([string]$Message) {
    $log.WriteLine(('{0:o} {1}ms {2}' -f [DateTime]::UtcNow, $clock.ElapsedMilliseconds, $Message))
}

function Exchange([byte]$Id, [byte]$Command, [int]$ReplyLength, [byte[]]$Payload = @()) {
    [byte[]]$header = @(0x3E, $Command, $Id, $Payload.Length)
    [byte[]]$frame = $header + (Get-Checksum $header)
    if ($Payload.Length) { $frame += $Payload + (Get-Checksum $Payload) }
    Write-Log ('TX ' + [BitConverter]::ToString($frame))
    $serial.Write($frame, 0, $frame.Length)
    $deadline = $clock.ElapsedMilliseconds + 150
    [byte[]]$reply = [byte[]]::new(5 + $ReplyLength + [int]($ReplyLength -gt 0))
    for ($i = 0; $i -lt $reply.Length; $i++) {
        $remaining = $deadline - $clock.ElapsedMilliseconds
        if ($remaining -le 0) { throw "Reply timeout, ID $Id." }
        $serial.ReadTimeout = [int]$remaining
        $reply[$i] = [byte]$serial.ReadByte()
    }
    Write-Log ('RX ' + [BitConverter]::ToString($reply))
    if ($reply[0] -ne 0x3E -or $reply[1] -ne $Command -or $reply[2] -ne $Id -or
        $reply[3] -ne $ReplyLength -or (Get-Checksum $reply[0..3]) -ne $reply[4]) {
        throw "Unexpected reply header, ID $Id."
    }
    if ($ReplyLength -and (Get-Checksum $reply[5..(4 + $ReplyLength)]) -ne $reply[-1]) {
        throw "Reply checksum error, ID $Id."
    }
    return ,$reply
}

function Read-Angle([int]$Id) {
    $reply = Exchange $Id 0x92 8
    $counts = [BitConverter]::ToInt64($reply, 5)
    Write-Log "ANGLE id=$Id raw=$counts"
    return $counts
}

function Read-Both {
    [long[]]$angles = @((Read-Angle $shoulderId), (Read-Angle $elbowId))
    return ,$angles
}

function Stop-Axis([int]$Id) {
    $serial.DiscardInBuffer()
    $null = Exchange $Id 0x80 0
}

function Measure-Direction([int]$Id, [string]$JointName) {
    while ($true) {
        $before = Read-Angle $Id
        $inputText = Read-Host "Turn $JointName CCW about 10 deg as seen from above. Hold it there and press Enter (Q to quit)"
        if ($inputText -eq 'Q') { throw 'Calibration cancelled.' }
        $after = Read-Angle $Id
        $change = Get-WrappedDeltaCounts $before $after
        Write-Host ('ID {0} measured change: {1:+0.00;-0.00;0.00} deg' -f $Id, ($change / 100.0))
        if ([math]::Abs($change) -ge 300 -and [math]::Abs($change) -le 2000) {
            return [math]::Sign($change)
        }
        Write-Warning 'Expected 3 to 20 deg. Try this joint again from its current position.'
    }
}

function Test-Supply([int]$Id) {
    $status = Exchange $Id 0x9A 7
    $voltage = [BitConverter]::ToUInt16($status, 7) / 10.0
    if ($voltage -lt 19 -or $voltage -gt 21 -or ($status[9] -bor $status[10] -bor $status[11]) -ne 0) {
        throw "Unexpected supply voltage or fault status on ID $Id."
    }
}

function Get-JointPlan([long[]]$Start, [long[]]$Target) {
    $moves = for ($index = 0; $index -lt 2; $index++) {
        $delta = $Target[$index] - $Start[$index]
        if ([math]::Abs($delta) -le 20) { continue }
        $count = [int][math]::Ceiling([math]::Abs($delta) / 200.0)
        for ($i = 1; $i -le $count; $i++) {
            $fraction = $i / [double]$count
            [pscustomobject]@{
                Index = $index
                Fraction = $fraction
                Target = [long][math]::Round($Start[$index] + $fraction * $delta)
            }
        }
    }
    $moves | Sort-Object Fraction, Index
}

function Show-JointPlan($Plan, [long[]]$Start) {
    for ($index = 0; $index -lt 2; $index++) {
        $axisPlan = @($Plan | Where-Object Index -eq $index)
        if ($axisPlan.Count) {
            $id = if ($index -eq 0) { $shoulderId } else { $elbowId }
            $first = ($axisPlan[0].Target - $Start[$index]) / 100.0
            Write-Host ('ID {0}: {1} commands; first step {2:+0.00;-0.00;0.00} deg.' -f $id, $axisPlan.Count, $first)
        }
    }
}

function Move-Step([int]$Index, [long]$Target) {
    $id = if ($Index -eq 0) { $shoulderId } else { $elbowId }
    $before = Read-Both
    $distance = $Target - $before[$Index]
    if ([math]::Abs($distance) -le 20) { return }
    if ([math]::Abs($distance) -gt 230) { throw "ID $id step exceeds 2.30 degrees." }
    $other = 1 - $Index
    $direction = [math]::Sign($distance)
    [byte[]]$payload = [BitConverter]::GetBytes($Target) + [BitConverter]::GetBytes([uint32]300)
    $state.lastAxis = $id
    Write-Log "TARGET id=$id raw=$Target initial=$($before[$Index])"
    $null = Exchange $id 0xA4 7 $payload
    $deadline = $clock.ElapsedMilliseconds + 5000
    $stableSince = $null
    while ($clock.ElapsedMilliseconds -lt $deadline) {
        $now = Read-Both
        if ([math]::Abs($now[$other] - $before[$other]) -ge 50) {
            throw 'The other joint moved by at least 0.50 deg.'
        }
        if ($direction * ($now[$Index] - $before[$Index]) -le -50) {
            throw "ID $id went at least 0.50 deg in the wrong direction."
        }
        if ($direction * ($now[$Index] - $Target) -ge 100) {
            throw "ID $id exceeded the step target by at least 1.00 deg."
        }
        if ([math]::Abs($Target - $now[$Index]) -le 20) {
            if ($null -eq $stableSince) { $stableSince = $clock.ElapsedMilliseconds }
            if ($clock.ElapsedMilliseconds - $stableSince -ge 250) { return }
        } else {
            $stableSince = $null
        }
        Start-Sleep -Milliseconds 20
    }
    throw "ID $id did not settle within 5 seconds."
}

function Show-Position([long[]]$Angles) {
    $shoulder = $cal.shoulderSign * (Get-WrappedDeltaCounts $cal.shoulderZero $Angles[0]) / 100.0
    $a = ($shoulder + $workspaceRotationDeg) * [math]::PI / 180
    $b = $cal.elbowSign * (Get-WrappedDeltaCounts $cal.elbowZero $Angles[1]) / 100.0 * [math]::PI / 180
    $px = $armLength * ([math]::Cos($a) + [math]::Cos($a + $b))
    $py = $armLength * ([math]::Sin($a) + [math]::Sin($a + $b))
    Write-Host ('Encoder-estimated XY: ({0:F2}, {1:F2}) mm' -f $px, $py)
}

function Get-RawTargets([long[]]$Angles, [double[]]$Solution) {
    $shoulderOffset = Get-WrappedDeltaCounts $cal.shoulderZero $Angles[0]
    $elbowOffset = Get-WrappedDeltaCounts $cal.elbowZero $Angles[1]
    $shoulderDesired = [long][math]::Round($cal.shoulderSign * $Solution[0] * 100)
    $elbowDesired = [long][math]::Round($cal.elbowSign * $Solution[1] * 100)
    [long[]]$targets = @(
        ($Angles[0] + (Get-WrappedDeltaCounts $shoulderOffset $shoulderDesired))
        ($Angles[1] - $elbowOffset + $elbowDesired)
    )
    return ,$targets
}

if ($Mode -ne 'Calibrate') {
    $solution = Solve-Point
    if ($Mode -eq 'Solve') {
        [pscustomobject]@{ Shoulder = $solution[0]; Elbow = $solution[1] }
        return
    }
    Write-Host ('Target XY ({0:F2}, {1:F2}) mm. ID 2: {2:F2} deg; ID 1: {3:F2} deg.' -f $X, $Y, $solution[0], $solution[1])
    if ($Mode -eq 'Math') { return }
    if (-not (Test-Path -LiteralPath $configPath)) { throw 'Run -Mode Calibrate first.' }
    $cal = Get-Content -Raw -LiteralPath $configPath | ConvertFrom-Json
    if ($cal.shoulderSign -notin -1, 1 -or $cal.elbowSign -notin -1, 1 -or
        $null -eq $cal.shoulderZero -or $null -eq $cal.elbowZero) { throw 'Calibration file is invalid.' }
}

[void](New-Item -ItemType Directory -Force -Path $logs)
$logPath = Join-Path $logs ('XY_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '.txt')
$log = [IO.StreamWriter]::new($logPath)
$log.AutoFlush = $true
$clock = [Diagnostics.Stopwatch]::StartNew()
$serial = [IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.WriteTimeout = 150
$state = @{ lastAxis = 0 }
try {
    $serial.Open()
    $serial.DiscardInBuffer()
    if ($Mode -eq 'Calibrate') {
        Stop-Axis $elbowId
        Stop-Axis $shoulderId
        $null = Read-Host 'Align both straight links with the marked +Y line (away from the operator), then press Enter'
        $reference = Read-Both
        Write-Host ('Recorded +Y reference: ID 2 = {0:F2} deg; ID 1 = {1:F2} deg.' -f ($reference[0] / 100.0), ($reference[1] / 100.0))
        $shoulderSign = Measure-Direction $shoulderId 'the shoulder (ID 2)'
        $elbowSign = Measure-Direction $elbowId 'the elbow (ID 1) relative to the stationary shoulder'
        $cal = [pscustomobject]@{
            shoulderZero = $reference[0] - $shoulderSign * 9000
            elbowZero = $reference[1]
            shoulderSign = $shoulderSign
            elbowSign = $elbowSign
        }
        $cal | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $configPath
        Write-Host "Calibration saved: $configPath"
        return
    }

    $start = Read-Both
    $target = Get-RawTargets $start $solution
    Show-Position $start
    Write-Host ('Raw travel, ID 2: {0:F2} -> {1:F2} deg; ID 1: {2:F2} -> {3:F2} deg.' -f
        ($start[0] / 100), ($target[0] / 100), ($start[1] / 100), ($target[1] / 100))
    $plan = @(Get-JointPlan $start $target)
    Show-JointPlan $plan $start
    if ($Mode -eq 'Plan') { return }

    $null = Read-Host 'Confirm the sweep is clear and the power switch is reachable. Press Enter to start, or Ctrl+C'
    Test-Supply $shoulderId
    Test-Supply $elbowId
    Stop-Axis $elbowId
    Stop-Axis $shoulderId
    $latest = Read-Both
    if ([math]::Abs((Get-WrappedDeltaCounts $start[0] $latest[0])) -gt 30 -or
        [math]::Abs((Get-WrappedDeltaCounts $start[1] $latest[1])) -gt 30) {
        throw 'A joint moved since the displayed plan. Re-run Plan.'
    }
    $start = $latest
    $target = Get-RawTargets $start $solution
    $plan = @(Get-JointPlan $start $target)
    for ($i = 0; $i -lt $plan.Count; $i++) {
        $move = $plan[$i]
        Move-Step $move.Index $move.Target
        Write-Host "Step $($i + 1) / $($plan.Count)"
    }
    $final = Read-Both
    Show-Position $final
    if ([math]::Abs($target[0] - $final[0]) -gt 20 -or [math]::Abs($target[1] - $final[1]) -gt 20) {
        throw 'Final joint angles are outside the 0.20 deg tolerance.'
    }
} finally {
    if ($serial.IsOpen -and $state.lastAxis) {
        foreach ($id in @($state.lastAxis, (3 - $state.lastAxis))) {
            try { Stop-Axis $id; Write-Host "ID $id shutdown acknowledged." }
            catch { Write-Warning "No shutdown acknowledgement from ID $id. Switch off motor power." }
        }
    }
    $serial.Dispose()
    $log.Dispose()
    Write-Host "Log: $logPath"
}
