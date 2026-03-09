param(
    [string]$InfPath = ".\kernelmode\vgpu_wddm\x64\Release\mvisor_wddm\mvisor_wddm.inf",
    [int]$SetupApiTail = 250,
    [switch]$Reboot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin)) {
    throw "Run this script in an elevated PowerShell window (Run as Administrator)."
}

$resolvedInf = Resolve-Path -Path $InfPath -ErrorAction Stop
Write-Host "[INFO] INF: $resolvedInf"

Write-Host "[INFO] Enumerating installed OEM driver packages..."
$driverLines = & pnputil /enum-drivers

$currentPublished = $null
$mvisorOemInfs = New-Object System.Collections.Generic.List[string]

foreach ($line in $driverLines) {
    if ($line -match "^\s*Published Name\s*:\s*(oem\d+\.inf)\s*$") {
        $currentPublished = $Matches[1].ToLowerInvariant()
        continue
    }

    if ($line -match "^\s*Original Name\s*:\s*(.+)\s*$") {
        if ($null -ne $currentPublished) {
            $originalName = $Matches[1].Trim().ToLowerInvariant()
            if ($originalName -eq "mvisor_wddm.inf") {
                $mvisorOemInfs.Add($currentPublished)
            }
        }
        $currentPublished = $null
    }
}

$mvisorOemInfs = $mvisorOemInfs | Sort-Object -Unique
if ($mvisorOemInfs.Count -gt 0) {
    Write-Host "[INFO] Removing old mvisor_wddm packages: $($mvisorOemInfs -join ', ')"
    foreach ($oemInf in $mvisorOemInfs) {
        & pnputil /delete-driver $oemInf /uninstall /force
    }
} else {
    Write-Host "[INFO] No old mvisor_wddm OEM packages found."
}

Write-Host "[INFO] Installing new package..."
& pnputil /add-driver $resolvedInf /install

Write-Host ""
Write-Host "[INFO] Display-class devices:"
& pnputil /enum-devices /class Display

Write-Host ""
Write-Host "[INFO] Win32_VideoController:"
Get-CimInstance Win32_VideoController |
    Select-Object Name, Status, DriverVersion, PNPDeviceID |
    Format-Table -AutoSize

Write-Host ""
Write-Host "[INFO] setupapi.dev.log tail ($SetupApiTail lines):"
Get-Content C:\Windows\INF\setupapi.dev.log -Tail $SetupApiTail

if ($Reboot) {
    Write-Host "[INFO] Rebooting in 3 seconds..."
    Start-Sleep -Seconds 3
    shutdown /r /t 0
} else {
    Write-Host "[INFO] Reboot is recommended after install."
}

