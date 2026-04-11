# Serial monitor for MokyaLora bringup shell (auto-detected COM port, 115200 baud)
# Usage:
#   .\scripts\serial_monitor.ps1            -- open shell, type commands manually
#   .\scripts\serial_monitor.ps1 key        -- send 'key' command then monitor
#   .\scripts\serial_monitor.ps1 "scan_a"   -- send any command then monitor
#
# Options:
#   -PortName COMxx    -- override auto-detection
#
# Press Ctrl+C to exit.

param(
    [string]$SendCmd = "",
    [string]$PortName = ""
)

. "$PSScriptRoot\_mokya-port.ps1"
$PortName = Resolve-MokyaPort $PortName

$port = New-Object System.IO.Ports.SerialPort $PortName, 115200, 'None', 8, 'One'
$port.DtrEnable = $true
$port.RtsEnable = $true
$port.ReadTimeout  = 100
$port.WriteTimeout = 1000
$port.NewLine = "`r`n"

try {
    $port.Open()
} catch {
    Write-Host "ERROR: Cannot open $PortName -- $_"
    exit 1
}

Write-Host "--- $PortName open (115200 8N1, DTR on) --- Ctrl+C to exit ---"
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
    Write-Host "`n--- $PortName closed ---"
}
