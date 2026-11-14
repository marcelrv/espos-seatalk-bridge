# Host (PC) unit tests for the pure-C SeaTalk components.
# Usage:  powershell -File tools\host-test.ps1 [-Clean]
# Configures into D:\Programming\esp\builds\host-test (outside the synced
# source tree), builds with MSVC + Ninja, runs ctest. Exit code != 0 on failure.
param([switch]$Clean)
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$build = 'D:\Programming\esp\builds\host-test'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'

$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'idf-env.ps1') *>$null
$ErrorActionPreference = 'Stop'

# Import the MSVC environment.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    $i = $_.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path "Env:$($_.Substring(0, $i))" -Value $_.Substring($i + 1) }
}

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

& cmake -G Ninja -DCMAKE_C_COMPILER=cl -S (Join-Path $root 'test\host') -B $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location $build
& ctest --output-on-failure
$rc = $LASTEXITCODE
Pop-Location
exit $rc
