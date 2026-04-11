# bringup_test_all.ps1 — Automated bringup regression test suite.
#
# Runs all automatable bringup commands over serial, validates output against
# expected patterns, and produces a pass/fail summary report.
#
# Usage:
#   .\scripts\bringup_test_all.ps1                  # run all tests (no flash)
#   .\scripts\bringup_test_all.ps1 -Flash           # build + flash + run all tests
#   .\scripts\bringup_test_all.ps1 -Group sensors   # run only one group
#   .\scripts\bringup_test_all.ps1 -Skip audio      # skip a group
#
# Groups: sensors, power, audio, memory, lora, display, core1
#
# Exit code: 0 = all pass, 1 = one or more failures

param(
    [switch]$Flash,
    [string]$PortName = '',
    [int]$Baud        = 115200,
    [string]$Group    = '',          # run only this group (empty = all)
    [string[]]$Skip   = @()          # skip these groups
)

. "$PSScriptRoot\_mokya-port.ps1"
$PortName = Resolve-MokyaPort $PortName

# ---------------------------------------------------------------------------
# Test definitions: [command, timeout_sec, description, regex_pass_pattern]
# A test passes if the output matches ALL listed patterns (regex, case-insensitive).
# ---------------------------------------------------------------------------

$Tests = @{
    'sensors' = @(
        @('imu',       2,  'IMU read',            @('LSM6DSV16X', 'Accel', 'Gyro', 'WHO_AM_I=0x70')),
        @('baro',      2,  'Barometer read',      @('LPS22HH', 'Pressure\s*:', 'hPa')),
        @('mag',       2,  'Magnetometer read',   @('LIS2MDL', 'X=', 'Y=', 'Z=')),
        @('gnss_info', 3,  'GNSS info',           @('Teseo|GNSS|\$GP|\$GN|PSTM')),
        @('scan_a',    2,  'I2C Bus A scan',      @('Scanning Bus A', 'Expected:.*0x6A.*0x1E.*0x5D.*0x3A', '@'))
    )
    'power' = @(
        @('scan_b',    2,  'I2C Bus B scan',      @('Scanning Bus B', 'Expected:.*0x6B', '@')),
        @('status',    2,  'Charger status',      @('BQ25622', 'CHG_STAT|VBUS_STAT|STATUS')),
        @('adc',       2,  'Charger ADC',         @('VBAT|VSYS|VBUS')),
        @('bq27441',  15,  'Fuel gauge',          @('BQ27441|CTRL_STATUS|Voltage|SOC'))
    )
    'audio' = @(
        @('amp_test',  7,  'Amplifier tone 5s',   @('NAU8315|amp|tone|Hz')),
        @('mic',       3,  'PDM mic capture',     @('PDM|density|bits'))
    )
    'memory' = @(
        @('sram',      2,  'Internal SRAM test',  @('SRAM|pattern|PASS|OK|pass|ok')),
        @('flash',     4,  'Flash JEDEC ID',      @('JEDEC|W25Q128|flash', 'Winbond|EF\s+60|PASS')),
        @('psram',     5,  'PSRAM 4KB test',      @('APS6404|PSRAM|pattern|PASS|OK|pass|ok')),
        @('psram_full',60, 'PSRAM full 8MB test', @('8\s*MB|full|PASS|OK|pass|ok|verify'))
    )
    'lora' = @(
        @('lora',      3,  'SX1262 basic test',   @('SX1262|GetStatus|SyncWord|0x[0-9A-Fa-f]+')),
        @('lora_dump', 8,  'SX1262 full dump',    @('SyncWord|OCP|RxGain|RSSI|stat'))
    )
    'display' = @(
        @('tft',      35,  'TFT colour fill',     @('ST7789|Red|Green|Blue|White|Black|init|fill')),
        @('tft_fast', 60,  'TFT speed test',      @('TE|FPS|DMA|clkdiv'))
    )
    # Single-element group: use ,@() to prevent PowerShell array flattening
    'core1' = @(,
        @('core1',     5,  'Core 1 test',         @('Core\s*1|FIFO|SRAM|GPIO|PASS|OK|pass|ok'))
    )
}

# Ordered group list for consistent execution
$GroupOrder = @('sensors', 'power', 'audio', 'memory', 'lora', 'display', 'core1')

# ---------------------------------------------------------------------------
# Step 0: build + flash (optional)
# ---------------------------------------------------------------------------
if ($Flash) {
    Write-Host "`n=== Building and flashing ===" -ForegroundColor Cyan
    & bash scripts/build_and_flash_bringup.sh
    if ($LASTEXITCODE -ne 0) { Write-Host 'Flash failed.' -ForegroundColor Red; exit 1 }
    Write-Host ''
}

# ---------------------------------------------------------------------------
# Step 1: open serial port
# ---------------------------------------------------------------------------
$serial = New-Object System.IO.Ports.SerialPort
$serial.PortName    = $PortName
$serial.BaudRate    = $Baud
$serial.Parity      = [System.IO.Ports.Parity]::None
$serial.DataBits    = 8
$serial.StopBits    = [System.IO.Ports.StopBits]::One
$serial.DtrEnable   = $true
$serial.RtsEnable   = $true
$serial.ReadTimeout = 300

$opened = $false
for ($try = 1; $try -le 5; $try++) {
    try {
        $serial.Open()
        $opened = $true
        break
    } catch {
        Write-Host "  Waiting for $PortName (attempt $try/5)..." -ForegroundColor Yellow
        Start-Sleep -Milliseconds 1000
    }
}
if (-not $opened) {
    Write-Host "ERROR: Cannot open $PortName after 5 attempts." -ForegroundColor Red
    exit 1
}
Write-Host "[$PortName opened at $Baud baud]" -ForegroundColor Green

function Recv([int]$ms) {
    $s = ''; $deadline = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $deadline) {
        try { $chunk = $serial.ReadExisting(); if ($chunk) { $s += $chunk } } catch {}
        Start-Sleep -Milliseconds 50
    }
    return $s
}

# Ping-pong: send command, wait for "> " prompt (firmware prints it after
# every command).  Falls back to timeout if prompt never arrives.
function SendCmd([string]$cmd, [int]$timeoutSec) {
    # Identical to bringup_run.ps1: send command, fixed-timeout Recv.
    # No ping-pong — USB CDC timing makes early-exit unreliable.
    $bytes = [System.Text.Encoding]::ASCII.GetBytes("$cmd`r")
    $serial.Write($bytes, 0, $bytes.Length)
    return Recv ($timeoutSec * 1000)
}

# ---------------------------------------------------------------------------
# Step 2: wait for boot banner, then sync prompt
# ---------------------------------------------------------------------------
Write-Host '[Waiting for boot banner...]' -ForegroundColor DarkGray
$banner = Recv 3500
if ($banner.Trim()) { Write-Host $banner }

# Robust prompt sync — send CR up to 5 times until we see "> "
# This handles: fresh boot, firmware stuck in serial mode, or mid-TFT-reinit.
$synced = $false
for ($syncTry = 1; $syncTry -le 5; $syncTry++) {
    try { $serial.ReadExisting() | Out-Null } catch {}
    $serial.Write([System.Text.Encoding]::ASCII.GetBytes("`r"), 0, 1)
    $resp = ''; $syncDeadline = (Get-Date).AddSeconds(2)
    while ((Get-Date) -lt $syncDeadline) {
        try { $chunk = $serial.ReadExisting(); if ($chunk) { $resp += $chunk } } catch {}
        if ($resp -match '>\s*$') { $synced = $true; break }
        Start-Sleep -Milliseconds 50
    }
    if ($synced) { break }
    Write-Host "  Sync attempt $syncTry/5..." -ForegroundColor Yellow
}
if (-not $synced) {
    Write-Host 'WARNING: Could not sync prompt — proceeding anyway.' -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Step 3: run tests
# ---------------------------------------------------------------------------
$results = @()
$totalPass = 0
$totalFail = 0
$totalSkip = 0
$startTime = Get-Date

Write-Host "`n" -NoNewline
Write-Host '======================================================' -ForegroundColor White
Write-Host '  MokyaLora Bringup Regression Test Suite' -ForegroundColor White
Write-Host '======================================================' -ForegroundColor White
Write-Host "  Date:   $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Write-Host "  Port:   $PortName @ $Baud"
Write-Host '======================================================' -ForegroundColor White
Write-Host ''

foreach ($grp in $GroupOrder) {
    # Filter by -Group / -Skip
    if ($Group -and $grp -ne $Group) { continue }
    if ($Skip -contains $grp) {
        Write-Host "  [$grp] SKIPPED" -ForegroundColor DarkGray
        $totalSkip += $Tests[$grp].Count
        continue
    }

    Write-Host "--- $($grp.ToUpper()) ---" -ForegroundColor Cyan

    $grpTests = $Tests[$grp]
    foreach ($t in $grpTests) {
        # Each $t is @(cmd, timeout, desc, patterns_array)
        # Guard against flattened single-element groups
        if ($t -is [string]) { break }
        $cmd      = $t[0]
        $timeout  = [int]$t[1]
        $desc     = $t[2]
        $patterns = @($t[3])

        Write-Host "  $cmd ($desc) ... " -NoNewline

        $out = SendCmd $cmd $timeout

        # Check all patterns
        $allMatch = $true
        $failedPat = @()
        foreach ($pat in $patterns) {
            if ($out -notmatch $pat) {
                $allMatch = $false
                $failedPat += $pat
            }
        }

        if ($allMatch) {
            Write-Host 'PASS' -ForegroundColor Green
            $totalPass++
            $results += [PSCustomObject]@{
                Group   = $grp
                Command = $cmd
                Result  = 'PASS'
                Detail  = ''
            }
        } else {
            Write-Host 'FAIL' -ForegroundColor Red
            $detail = "Missing patterns: $($failedPat -join ', ')"
            Write-Host "    $detail" -ForegroundColor DarkYellow
            # Print first 500 chars of output for debugging
            $preview = if ($out.Length -gt 500) { $out.Substring(0, 500) + '...' } else { $out }
            Write-Host "    Output: $($preview.Trim())" -ForegroundColor DarkGray
            $totalFail++
            $results += [PSCustomObject]@{
                Group   = $grp
                Command = $cmd
                Result  = 'FAIL'
                Detail  = $detail
            }
        }

        # (inter-command pause is inside SendCmd's Recv 500)
    }
    Write-Host ''
}

# ---------------------------------------------------------------------------
# Step 4: close serial
# ---------------------------------------------------------------------------
$serial.Close()
$elapsed = (Get-Date) - $startTime

# ---------------------------------------------------------------------------
# Step 5: summary report
# ---------------------------------------------------------------------------
Write-Host '======================================================' -ForegroundColor White
Write-Host '  SUMMARY' -ForegroundColor White
Write-Host '======================================================' -ForegroundColor White
Write-Host "  Total:   $($totalPass + $totalFail + $totalSkip)"
Write-Host "  Pass:    $totalPass" -ForegroundColor Green
if ($totalFail -gt 0) {
    Write-Host "  Fail:    $totalFail" -ForegroundColor Red
} else {
    Write-Host "  Fail:    0" -ForegroundColor Green
}
if ($totalSkip -gt 0) {
    Write-Host "  Skip:    $totalSkip" -ForegroundColor DarkGray
}
Write-Host "  Time:    $([math]::Round($elapsed.TotalSeconds, 1))s"
Write-Host '======================================================' -ForegroundColor White

# Print failed tests
if ($totalFail -gt 0) {
    Write-Host "`nFailed tests:" -ForegroundColor Red
    foreach ($r in $results) {
        if ($r.Result -eq 'FAIL') {
            Write-Host "  [$($r.Group)] $($r.Command) -- $($r.Detail)" -ForegroundColor Red
        }
    }
}

# Save results to log file
$logDir  = 'docs/bringup/measurements'
$logFile = "$logDir/test-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

$logContent = @(
    "MokyaLora Bringup Test Report"
    "Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "Duration: $([math]::Round($elapsed.TotalSeconds, 1))s"
    "Pass: $totalPass  Fail: $totalFail  Skip: $totalSkip"
    "---"
)
foreach ($r in $results) {
    $logContent += "$($r.Result)  [$($r.Group)] $($r.Command)  $($r.Detail)"
}
$logContent -join "`n" | Out-File -FilePath $logFile -Encoding utf8
Write-Host "`nLog saved: $logFile" -ForegroundColor DarkGray

Write-Host ''
if ($totalFail -eq 0) {
    Write-Host 'ALL TESTS PASSED' -ForegroundColor Green
    exit 0
} else {
    Write-Host 'SOME TESTS FAILED' -ForegroundColor Red
    exit 1
}
