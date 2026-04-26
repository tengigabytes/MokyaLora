param(
    [string]$Cmd = "",
    [int]$Seconds = 10,
    [string]$PortName = ""
)
. "$PSScriptRoot\_mokya-port.ps1"
$PortName = Resolve-MokyaPort $PortName
$port = New-Object System.IO.Ports.SerialPort $PortName, 115200, 'None', 8, 'One'
$port.DtrEnable = $true
$port.RtsEnable = $true
$port.ReadTimeout  = 100
$port.NewLine = "`r`n"
$port.Open()
Start-Sleep -Milliseconds 500
$port.DiscardInBuffer()
if ($Cmd -ne "") {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes("$Cmd`r`n")
    $port.Write($bytes, 0, $bytes.Length)
}
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    try {
        $data = $port.ReadExisting()
        if ($data.Length -gt 0) { Write-Host -NoNewline $data }
    } catch [System.TimeoutException] {}
    Start-Sleep -Milliseconds 50
}
$port.Close()
