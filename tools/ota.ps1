# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
# OTA update of a seatalk-bridge device from this PC.
#
#     .\tools\ota.ps1 -Device espos-f768.local
#     .\tools\ota.ps1 -Device 192.168.1.50 -ApiKey <httpd.api_key>   # once an API key is set
#
# espOS pulls updates from a URL, so this serves the signed image over HTTP
# from a temporary folder (only the .bin, nothing else of the build), asks the
# device to install it (POST /api/v1/ota) and follows /api/v1/ota/status
# until the device restarts. The image must be signed with the key the
# device was flashed with (secure_boot_signing_key.pem); espOS rejects
# anything else and keeps running the old firmware.
param(
    [Parameter(Mandatory = $true)][string]$Device,
    [string]$Image = 'D:\Programming\esp\builds\seatalk-bridge\seatalk_bridge.bin',
    [int]$Port = 8070,
    [string]$ApiKey = ''
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Image)) { throw "image not found: $Image (build first)" }

# Local address the device can reach us on: the interface that routes to it.
$devIp = [System.Net.Dns]::GetHostAddresses($Device) |
    Where-Object { $_.AddressFamily -eq 'InterNetwork' } | Select-Object -First 1
if (-not $devIp) { throw "cannot resolve $Device" }
$myIp = (Find-NetRoute -RemoteIPAddress $devIp.IPAddressToString |
    Where-Object { $_.IPAddress } | Select-Object -First 1).IPAddress
Write-Host "device $Device = $devIp, serving from $myIp`:$Port"

$serve = Join-Path ([System.IO.Path]::GetTempPath()) "seatalk-ota-$PID"
New-Item -ItemType Directory -Force $serve | Out-Null
$name = Split-Path $Image -Leaf
Copy-Item $Image (Join-Path $serve $name)

$py = (Get-Command python).Source
$srv = Start-Process -FilePath $py -ArgumentList @('-m', 'http.server', $Port, '--bind', $myIp, '--directory', $serve) `
    -PassThru -WindowStyle Hidden
$headers = @{}
if ($ApiKey) { $headers['Authorization'] = "Bearer $ApiKey" }
$base = "http://$Device/api/v1"
try {
    Start-Sleep -Milliseconds 800
    $body = @{ url = "http://$myIp`:$Port/$name" } | ConvertTo-Json -Compress
    $r = Invoke-RestMethod -Method Post -Uri "$base/ota" -Body $body -ContentType 'application/json' -Headers $headers
    Write-Host "install requested: $($r | ConvertTo-Json -Compress)"

    $deadline = (Get-Date).AddMinutes(5)
    $last = ''
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        try {
            $s = Invoke-RestMethod -Uri "$base/ota/status" -Headers $headers -TimeoutSec 5
        } catch {
            Write-Host 'device not answering (restarting?)'
            continue
        }
        $line = "state $($s.state)  $($s.progress.received)/$($s.progress.total)  running $($s.running.version) in $($s.running.slot)"
        if ($s.last_error) { $line += "  error: $($s.last_error)" }
        if ($line -ne $last) { Write-Host $line; $last = $line }
        if ($s.state -in @('ready', 'failed')) { break }
    }
    # After 'ready' the device restarts into the new slot ~1.5 s later.
    Start-Sleep -Seconds 15
    $s = Invoke-RestMethod -Uri "$base/ota/status" -Headers $headers -TimeoutSec 10
    Write-Host "now running $($s.running.version) in $($s.running.slot) (other: $($s.running.other_version), rolled_back $($s.running.rolled_back))"
} finally {
    Stop-Process -Id $srv.Id -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $serve -ErrorAction SilentlyContinue
}
