# Serial monitor for MokyaLora bringup shell (COM4, 115200 baud)
# Usage:
#   .\serial_monitor.ps1            -- open shell, type commands manually
#   .\serial_monitor.ps1 key        -- send 'key' command then monitor
#   .\serial_monitor.ps1 "scan_a"   -- send any command then monitor
#
# Press Ctrl+C to exit.

param(
    [string]$SendCmd = ""
)

$port = New-Object System.IO.Ports.SerialPort 'COM4', 115200, 'None', 8, 'One'
$port.DtrEnable = $true
$port.RtsEnable = $true
$port.ReadTimeout  = 100
$port.WriteTimeout = 1000
$port.NewLine = "`r`n"

try {
    $port.Open()
} catch {
    Write-Host "ERROR: Cannot open COM4 -- $_"
    exit 1
}

Write-Host "--- COM4 open (115200 8N1, DTR on) --- Ctrl+C to exit ---"
Start-Sleep -Milliseconds 600

if ($SendCmd -ne "") {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes("$SendCmd`r`n")
    $port.Write($bytes, 0, $bytes.Length)
    Write-Host "> $SendCmd"
}

try {
    while ($true) {
        try {
            $data = $port.ReadExisting()
            if ($data.Length -gt 0) {
                Write-Host -NoNewline $data
            }
        } catch [System.TimeoutException] {}
        Start-Sleep -Milliseconds 20
    }
} finally {
    $port.Close()
    Write-Host "`n--- COM4 closed ---"
}
