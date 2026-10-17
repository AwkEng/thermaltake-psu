#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Capture USB traffic to/from the PSU with USBPcap, with timestamped labels.
.EXAMPLE
  .\tools\capture\capture.ps1 -Name idle
  Type a label and press Enter each time you do something in the vendor app;
  press Enter on an empty line to stop.
#>
param(
    [Parameter(Mandatory)][string]$Name,
    [string]$Interface = '\\.\USBPcap2',
    [string]$Device,
    # Capture for a fixed time instead of prompting for labels.
    [int]$Seconds
)
$ErrorActionPreference = 'Stop'
$exe = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
$capDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\usbdumps')).Path

if (-not $Device) {
    # USBPcapCMD prints the device tree on stderr.
    $cfg = & $exe --extcap-interface $Interface --extcap-config 2>&1 | ForEach-Object { "$_" }
    $m = @($cfg | Select-String '\{display=HID-compliant vendor-defined device\}.*\{parent=(\d+)\}')
    if ($m.Count -ne 1) { throw "Couldn't auto-detect the PSU on $Interface; pass -Device <n>" }
    $Device = $m[0].Matches[0].Groups[1].Value
}

$n = @(Get-ChildItem $capDir -Filter '*.pcap').Count + 1
$base = Join-Path $capDir ('{0:D2}-{1}' -f $n, $Name)
$notes = "$base.txt"

$proc = Start-Process $exe -PassThru -WindowStyle Hidden `
    -ArgumentList "-d $Interface --devices $Device -o `"$base.pcap`""
"$(Get-Date -Format o)  START device=$Device" | Out-File $notes -Encoding utf8
Write-Host "Capturing device $Device on $Interface -> $base.pcap"

if ($Seconds) { Start-Sleep -Seconds $Seconds }
while (-not $Seconds) {
    $label = Read-Host 'label (empty line to stop)'
    if (-not $label) { break }
    "$(Get-Date -Format o)  $label" | Out-File $notes -Append -Encoding utf8
}

Start-Sleep -Seconds 2
"$(Get-Date -Format o)  STOP" | Out-File $notes -Append -Encoding utf8
Stop-Process -Id $proc.Id
Write-Host "Saved $base.pcap and $notes"
