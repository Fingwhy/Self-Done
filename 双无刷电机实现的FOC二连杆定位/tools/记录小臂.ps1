param(
    [string]$Port = 'COM10',
    [ValidateSet(1, 2)]
    [int]$MotorId = 1,
    [switch]$Move,
    [switch]$Reverse,
    [ValidateRange(0.5, 30.0)]
    [double]$Degrees = 2.0,
    [switch]$TraceOutput,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'joint_angles.ps1')
$motorName = if ($MotorId -eq 1) { 'MS7025' } else { 'MS7015' }
$stepMagnitude = [long][math]::Round($Degrees * 100)
$stepCounts = if ($Reverse) { -$stepMagnitude } else { $stepMagnitude }
$speedCounts = 300
$stopCounts = [math]::Max(500L, ($stepMagnitude + 100))
$wrongWayCounts = if ($TraceOutput) { 50 } else { 100 }
$durationMs = if ($Degrees -le 2) { 5000 } else {
    [int][math]::Ceiling($stepMagnitude * 1000.0 / $speedCounts) + 6000
}
$sampleMs = 30

function Get-Checksum([byte[]]$Bytes) {
    $total = 0
    foreach ($item in $Bytes) { $total += $item }
    return [byte]($total -band 255)
}

function New-Frame([byte]$Command, [byte[]]$Payload = @()) {
    [byte[]]$header = @(0x3E, $Command, $motorId, $Payload.Length)
    [byte[]]$result = $header + (Get-Checksum $header)
    if ($Payload.Length) { $result += $Payload + (Get-Checksum $Payload) }
    return ,$result
}

function Test-Frame([byte[]]$Frame) {
    if ($Frame.Length -lt 5 -or $Frame[0] -ne 0x3E -or $Frame[2] -ne $motorId) {
        throw 'Reply frame header or motor ID is invalid.'
    }
    $payloadLength = $Frame[3]
    $expectedLength = 5 + $payloadLength + [int]($payloadLength -gt 0)
    if ($Frame.Length -ne $expectedLength -or (Get-Checksum $Frame[0..3]) -ne $Frame[4]) {
        throw 'Reply frame length or header checksum is invalid.'
    }
    if ($payloadLength -and (Get-Checksum $Frame[5..(4 + $payloadLength)]) -ne $Frame[-1]) {
        throw 'Reply payload checksum is invalid.'
    }
}

function ConvertTo-Hex([byte[]]$Bytes) {
    return [BitConverter]::ToString($Bytes).Replace('-', ' ')
}

function New-PositionFrame([long]$TargetCounts) {
    [byte[]]$payload = [BitConverter]::GetBytes($TargetCounts) + [BitConverter]::GetBytes([uint32]$speedCounts)
    return ,(New-Frame 0xA4 $payload)
}

function Convert-DriveStatus([byte[]]$Reply) {
    [pscustomobject]@{
        Output = [BitConverter]::ToInt16($Reply, 6)
        Speed = [BitConverter]::ToInt16($Reply, 8)
        Encoder = [BitConverter]::ToUInt16($Reply, 10)
    }
}

if ($SelfTest) {
    if ($MotorId -eq 1) {
        $startCounts = -14633
        $expectedMove = if ($Reverse) {
            '3E A4 01 0C EF 0F C6 FF FF FF FF FF FF 2C 01 00 00 FC'
        } else {
            '3E A4 01 0C EF 9F C7 FF FF FF FF FF FF 2C 01 00 00 8D'
        }
        $expectedStop = '3E 80 01 00 BF'
        [byte[]]$reply = @(0x3E, 0x92, 1, 8, 0xD9, 0xD7, 0xC6, 255, 255, 255, 255, 255, 255, 0x97)
    } else {
        $startCounts = 12454
        $expectedMove = if ($Reverse) {
            '3E A4 02 0C F0 DE 2F 00 00 00 00 00 00 2C 01 00 00 3A'
        } else {
            '3E A4 02 0C F0 6E 31 00 00 00 00 00 00 2C 01 00 00 CC'
        }
        $expectedStop = '3E 80 02 00 C0'
        [byte[]]$reply = @(0x3E, 0x92, 2, 8, 0xDA, 0xA6, 0x30, 0, 0, 0, 0, 0, 0, 0xD6)
    }
    $knownStep = if ($Reverse) { -200 } else { 200 }
    if ((ConvertTo-Hex (New-PositionFrame ($startCounts + $knownStep))) -ne $expectedMove) {
        throw 'A4 does not match the known command.'
    }
    if ($MotorId -eq 1 -and $Reverse -and $Degrees -eq 5 -and
        (ConvertTo-Hex (New-PositionFrame ($startCounts + $stepCounts))) -ne
        '3E A4 01 0C EF E3 C4 FF FF FF FF FF FF 2C 01 00 00 CE') {
        throw 'Five-degree reverse A4 does not match the expected frame.'
    }
    if ($MotorId -eq 1 -and $Reverse -and $Degrees -eq 30 -and
        ((ConvertTo-Hex (New-PositionFrame ($startCounts + $stepCounts))) -ne
         '3E A4 01 0C EF 1F BB FF FF FF FF FF FF 2C 01 00 00 01' -or
         $stopCounts -ne 3100 -or $durationMs -ne 16000)) {
        throw 'Thirty-degree reverse A4 or trace limits do not match the test settings.'
    }
    if ((ConvertTo-Hex (New-Frame 0x80)) -ne $expectedStop) { throw 'Stop frame is incorrect.' }
    Test-Frame $reply
    if ([BitConverter]::ToInt64($reply, 5) -ne $startCounts) { throw 'Angle decode is incorrect.' }
    $reply[5] = 0
    $rejected = $false
    try { Test-Frame $reply } catch { $rejected = $true }
    if (-not $rejected) { throw 'A bad checksum was accepted.' }
    [byte[]]$statusReply = @(0x3E, 0xA4, 2, 7, 0xEB, 0x18, 0xB6, 0xFF, 0x0B, 0, 0x88, 5, 0x65)
    $trace = Convert-DriveStatus $statusReply
    if ($trace.Output -ne -74 -or $trace.Speed -ne 11 -or $trace.Encoder -ne 1416) {
        throw 'Status output, velocity, or encoder decoding is incorrect.'
    }
    Write-Host 'Offline checks passed. No serial port was opened.'
    return
}
if ($Reverse -and -not $Move) { throw 'Use -Move -Reverse to test a reverse step.' }
if ($PSBoundParameters.ContainsKey('Degrees') -and -not $Move) { throw 'Use -Move to test a custom angle.' }

function Write-Log([string]$Kind, [string]$Event, [string]$Hex = '', [string]$Angle = '', [string]$Delta = '',
                   [string]$Output = '', [string]$Speed = '', [string]$Encoder = '') {
    $utc = [DateTime]::UtcNow.ToString('o')
    $log.WriteLine("$utc,$($clock.ElapsedMilliseconds),$Kind,$Event,$Hex,$Angle,$Delta,$Output,$Speed,$Encoder")
}

function Send-Frame([byte[]]$Frame) {
    Write-Log 'TX_ATTEMPT' 'serial_write' (ConvertTo-Hex $Frame)
    $serial.Write($Frame, 0, $Frame.Length)
    Write-Log 'TX' 'serial_write_ok' (ConvertTo-Hex $Frame)
}

function Invoke-Frame([byte[]]$Frame, [byte]$ExpectedPayload) {
    Send-Frame $Frame
    $deadline = $clock.ElapsedMilliseconds + 150
    $buffer = [System.Collections.Generic.List[byte]]::new()
    $frameLength = 5
    while ($buffer.Count -lt $frameLength) {
        $remaining = $deadline - $clock.ElapsedMilliseconds
        if ($remaining -le 0) { throw 'Timed out receiving a complete frame.' }
        $serial.ReadTimeout = [int]$remaining
        $buffer.Add([byte]$serial.ReadByte())
        if ($buffer.Count -eq 5) {
            $length = $buffer[3]
            if ($length -gt 32) { throw 'Unexpected reply payload length.' }
            $frameLength = 5 + $length + [int]($length -gt 0)
        }
    }
    [byte[]]$reply = $buffer.ToArray()
    Write-Log 'RX' 'serial_read' (ConvertTo-Hex $reply)
    Test-Frame $reply
    if ($reply[1] -ne $Frame[1] -or $reply[3] -ne $ExpectedPayload) {
        throw 'Unexpected reply command or payload length.'
    }
    return ,$reply
}

function Read-Angle {
    $reply = Invoke-Frame (New-Frame 0x92) 8
    return [BitConverter]::ToInt64($reply, 5)
}

function Write-DriveTrace([string]$Event, [byte[]]$Reply) {
    $trace = Convert-DriveStatus $Reply
    Write-Log 'STATUS' $Event (ConvertTo-Hex $Reply) '' '' $trace.Output $trace.Speed $trace.Encoder
}

function Read-DriveTrace([string]$Event) {
    $reply = Invoke-Frame (New-Frame 0x9C) 7
    Write-DriveTrace $Event $reply
}

function Format-Deg([long]$Counts) {
    return ($Counts / 100.0).ToString('F2', [Globalization.CultureInfo]::InvariantCulture)
}

$folder = Join-Path $PSScriptRoot 'serial_logs'
[void](New-Item -ItemType Directory -Force -Path $folder)
$path = Join-Path $folder ($motorName + '_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '.csv')
$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.WriteTimeout = 150
$serial.Handshake = 'None'
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$log = [IO.StreamWriter]::new($path, $false, [Text.UTF8Encoding]::new($true))
$log.AutoFlush = $true
$clock = [Diagnostics.Stopwatch]::StartNew()
$motionAttempted = $false
try {
    $log.WriteLine('utc,t_ms,kind,event,frame_hex,angle_deg,delta_deg,output_raw,reported_speed_dps,encoder_raw')
    $serial.Open()
    $serial.DiscardInBuffer()
    $status = Invoke-Frame (New-Frame 0x9A) 7
    $voltage = [BitConverter]::ToUInt16($status, 7) / 10.0
    $baseline = @()
    for ($i = 0; $i -lt 10; $i++) {
        $angle = Read-Angle
        $baseline += $angle
        Write-Log 'SAMPLE' 'baseline' '' (Format-Deg $angle)
        if ($TraceOutput) { Read-DriveTrace 'baseline_status' }
        Start-Sleep -Milliseconds $sampleMs
    }
    $statistics = $baseline | Measure-Object -Minimum -Maximum
    if ($statistics.Maximum - $statistics.Minimum -gt 30) {
        throw 'Baseline changed by more than 0.30 deg. Check that the arm is still.'
    }
    $start = [long]$baseline[-1]
    Write-Host "ID $MotorId is responding. Bus voltage: $voltage V. Angle: $(Format-Deg $start) deg."
    if (-not $Move) { return }

    if ($voltage -lt 19 -or $voltage -gt 21 -or ($status[9] -bor $status[10] -bor $status[11]) -ne 0) {
        throw 'Supply voltage or fault status is outside the test conditions.'
    }
    $null = Invoke-Frame (New-Frame 0x80) 0
    $start = Read-Angle
    if ([math]::Abs((Get-WrappedDeltaCounts ([long]$baseline[-1]) $start)) -gt 30) {
        throw 'Position shifted after drive shutdown. No move sent.'
    }
    $target = $start + $stepCounts
    Write-Host "ID $MotorId requested travel: $(Format-Deg $stepCounts) deg. Target: $(Format-Deg $target) deg."
    Write-Log 'EVENT' 'target' '' (Format-Deg $target) (Format-Deg $stepCounts)
    if ($TraceOutput) { Read-DriveTrace 'before_a4' }
    $motionAttempted = $true
    $ack = Invoke-Frame (New-PositionFrame $target) 7
    if ($TraceOutput) { Write-DriveTrace 'a4_ack_status' $ack }
    $moveAt = $clock.ElapsedMilliseconds
    $withinSince = $null
    while ($clock.ElapsedMilliseconds - $moveAt -lt $durationMs) {
        $nextSample = $clock.ElapsedMilliseconds + $sampleMs
        $angle = Read-Angle
        $delta = $angle - $start
        Write-Log 'SAMPLE' 'motion' '' (Format-Deg $angle) (Format-Deg $delta)
        if ([math]::Abs($delta) -ge $stopCounts) {
            Write-Log 'EVENT' 'travel_threshold'
            Write-Warning "Reported movement reached $(Format-Deg $delta) deg. Shutting down drive."
            break
        }
        if ($delta * [math]::Sign($stepCounts) -le -$wrongWayCounts) {
            Write-Log 'EVENT' 'wrong_direction_threshold'
            Write-Warning "Reported motion reached $(Format-Deg $delta) deg in the wrong direction. Shutting down drive."
            break
        }
        if ($TraceOutput -and (($angle - $target) * [math]::Sign($stepCounts) -ge 100)) {
            Write-Log 'EVENT' 'overshoot_threshold'
            Write-Warning 'Reported angle passed the target by at least 1.00 deg. Shutting down drive.'
            break
        }
        if ($TraceOutput) { Read-DriveTrace 'motion_status' }
        if ([math]::Abs($target - $angle) -le 20) {
            if ($null -eq $withinSince) { $withinSince = $clock.ElapsedMilliseconds }
            if ($clock.ElapsedMilliseconds - $withinSince -ge 350) {
                Write-Log 'EVENT' 'settled'
                break
            }
        } else {
            $withinSince = $null
        }
        $delay = $nextSample - $clock.ElapsedMilliseconds
        if ($delay -gt 0) { Start-Sleep -Milliseconds $delay }
    }
    if ($clock.ElapsedMilliseconds - $moveAt -ge $durationMs) {
        Write-Log 'EVENT' 'test_timeout'
    }
} finally {
    try {
        if ($motionAttempted -and $serial.IsOpen) {
            $shutdownConfirmed = $false
            try {
                $serial.DiscardInBuffer()
                $null = Invoke-Frame (New-Frame 0x80) 0
                $shutdownConfirmed = $true
                Write-Host "ID $MotorId shutdown acknowledged."
            } catch {
                Write-Warning 'Drive shutdown acknowledgement missing. Switch off the motor power.'
            }
            if ($shutdownConfirmed) {
                try {
                    for ($i = 0; $i -lt 10; $i++) {
                        $angle = Read-Angle
                        Write-Log 'SAMPLE' 'after_shutdown' '' (Format-Deg $angle) (Format-Deg (Get-WrappedDeltaCounts $start $angle))
                        Start-Sleep -Milliseconds 50
                    }
                } catch {
                    Write-Log 'EVENT' 'after_shutdown_read_failed'
                    Write-Warning 'Could not finish sampling the angle after shutdown.'
                }
            }
        }
    } finally {
        $serial.Dispose()
        $log.Dispose()
        Write-Host "Log: $path"
    }
}
