# read_freertos.ps1 — Flash core1_freertos_test then capture serial output.
# Uses Write-Output (not Write-Host) so output is captured by 2>&1 redirect.
# COM port is always closed in finally block.

param(
    [string]$Port = 'COM4',
    [int]$ReadSeconds = 14
)

# --- 1. Build + flash (resets MCU) ---
Write-Output '=== Building and flashing core1_freertos_test ==='
& bash flash_core1_freertos.sh
if ($LASTEXITCODE -ne 0) { Write-Output 'Flash failed.'; exit 1 }

# --- 2. Open COM port (retry while USB re-enumerates after reset) ---
$serial = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$serial.DtrEnable   = $true
$serial.RtsEnable   = $true
$serial.ReadTimeout = 300

$opened = $false
for ($i = 1; $i -le 10; $i++) {
    try { $serial.Open(); $opened = $true; break }
    catch { Write-Output "  Waiting for $Port (attempt $i/10)..."; Start-Sleep -Milliseconds 600 }
}
if (-not $opened) { Write-Output "ERROR: cannot open $Port"; exit 1 }
Write-Output "[$Port open - reading ${ReadSeconds}s]"

# --- 3. Read, always close when done ---
try {
    $buf = [System.Text.StringBuilder]::new()
    $end = [DateTime]::Now.AddSeconds($ReadSeconds)
    while ([DateTime]::Now -lt $end) {
        try {
            $d = $serial.ReadExisting()
            if ($d.Length -gt 0) { [void]$buf.Append($d) }
        } catch {}
        Start-Sleep -Milliseconds 50
    }
    Write-Output $buf.ToString()
} finally {
    $serial.Close()
    Write-Output "[$Port closed]"
}
