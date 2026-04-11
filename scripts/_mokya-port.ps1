# _mokya-port.ps1 — Auto-detect RP2350B USB CDC serial port by VID.
#
# Dot-source from other scripts:
#   . "$PSScriptRoot\_mokya-port.ps1"
#   $PortName = Resolve-MokyaPort $PortName
#
# RP2350B (Pico SDK stdio_usb) enumerates with VID 0x2E8A (Raspberry Pi
# Foundation). COM number varies per host and per USB enumeration order.

function Find-MokyaPort {
    $dev = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -match 'VID_2E8A' } |
           Select-Object -First 1
    if ($dev -and $dev.FriendlyName -match '\(COM(\d+)\)') {
        return "COM$($matches[1])"
    }
    return $null
}

function Resolve-MokyaPort {
    param([string]$PortName)
    if ($PortName) { return $PortName }
    $auto = Find-MokyaPort
    if ($auto) {
        Write-Host "[auto-detected $auto]" -ForegroundColor DarkGray
        return $auto
    }
    Write-Host "ERROR: No RP2350B serial port found (VID 2E8A). Pass -PortName COMxx to override." -ForegroundColor Red
    exit 1
}
