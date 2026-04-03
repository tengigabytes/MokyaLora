# bringup_run.ps1 — Send one or more commands to the bringup shell and print output.
#
# Usage:
#   .\bringup_run.ps1 psram
#   .\bringup_run.ps1 -Flash psram
#   .\bringup_run.ps1 scan_a scan_b status lora lora_dump flash psram
#   .\bringup_run.ps1 -Flash scan_a scan_b status lora lora_dump flash psram
#
# Options:
#   -Flash      Build + flash via J-Link before connecting (triggers MCU reset)
#   -PortName   Serial port name (default: COM4)
#   -Baud       Baud rate (default: 115200)
#
# Per-command timeouts (seconds) — extend for slow operations:
#   lora_dump 8 s, lora_rx 35 s, psram 5 s, others 2 s

param(
    [switch]$Flash,
    [Parameter(ParameterSetName='Default')]
    [string]$PortName = 'COM4',
    [Parameter(ParameterSetName='Default')]
    [int]$Baud        = 115200,
    [Parameter(Position=0, ValueFromRemainingArguments=$true)]
    [string[]]$Commands = @('help')
)

# Per-command receive timeout table (seconds)
$CmdTimeout = @{
    'lora_rx'   = 35
    'lora_dump' = 8
    'psram'     = 5
    'lora'      = 3
    'gnss_info' = 3
    'led'       = 12
    'motor'     = 12
    'amp'       = 12
    'bee'       = 10
    'amp_test'  = 7
    'mic'       = 3
    'mic_raw'   = 12
    'mic_loop'  = 12
    'tft'       = 15
}
$DefaultTimeout = 2

# ---------------------------------------------------------------------------
# Step 1: build + flash (optional)
# ---------------------------------------------------------------------------
if ($Flash) {
    Write-Host '=== Building and flashing ===' -ForegroundColor Cyan
    & bash build_and_flash_bringup.sh
    if ($LASTEXITCODE -ne 0) { Write-Host 'Flash failed.' -ForegroundColor Red; exit 1 }
    Write-Host ''
}

# ---------------------------------------------------------------------------
# Step 2: open COM port
# ---------------------------------------------------------------------------
# After J-Link reset the USB CDC re-enumerates; retry a few times.
# NOTE: variable named $serial to avoid case-insensitive clash with $PortName param.
$serial = New-Object System.IO.Ports.SerialPort
$serial.PortName    = $PortName
$serial.BaudRate    = $Baud
$serial.Parity      = [System.IO.Ports.Parity]::None
$serial.DataBits    = 8
$serial.StopBits    = [System.IO.Ports.StopBits]::One
$serial.DtrEnable   = $true
$serial.RtsEnable   = $true
$serial.ReadTimeout = 300
# WriteTimeout left at default (-1 = infinite) to avoid spurious timeout errors.

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

# ---------------------------------------------------------------------------
# Helper: collect bytes for $ms milliseconds
# ---------------------------------------------------------------------------
function Recv([int]$ms) {
    $s = ''; $deadline = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $deadline) {
        try { $chunk = $serial.ReadExisting(); if ($chunk) { $s += $chunk } } catch {}
        Start-Sleep -Milliseconds 50
    }
    return $s
}

# ---------------------------------------------------------------------------
# Step 3: wait for banner (firmware has 2 s boot delay + charge_off ~200 ms)
# ---------------------------------------------------------------------------
Write-Host '[Waiting for boot banner...]' -ForegroundColor DarkGray
$banner = Recv 3500
if ($banner.Trim()) { Write-Host $banner }

# Send an empty line to ensure we have a fresh prompt
$serial.Write([System.Text.Encoding]::ASCII.GetBytes("`r"), 0, 1)
Recv 300 | Out-Null

# ---------------------------------------------------------------------------
# Step 4: run each command
# ---------------------------------------------------------------------------
foreach ($cmd in $Commands) {
    $timeoutSec = if ($CmdTimeout.ContainsKey($cmd)) { $CmdTimeout[$cmd] } else { $DefaultTimeout }
    Write-Host "=== $cmd (timeout ${timeoutSec}s) ===" -ForegroundColor Cyan

    $bytes = [System.Text.Encoding]::ASCII.GetBytes("$cmd`r")
    $serial.Write($bytes, 0, $bytes.Length)
    $out = Recv ($timeoutSec * 1000)
    Write-Host $out
}

# ---------------------------------------------------------------------------
# Step 5: close
# ---------------------------------------------------------------------------
$serial.Close()
Write-Host "[$PortName closed]" -ForegroundColor Green
