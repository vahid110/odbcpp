# Run before packaging: negative PE checks and schema safeguards need no installer.
param([string]$BuildDirectory = 'build-windows')
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:RUNNER_TEMP 'odbcpp-invalid-package'
New-Item "$root/Release" -ItemType Directory -Force | Out-Null
Copy-Item "$BuildDirectory/CMakeCache.txt" $root
$bytes = [IO.File]::ReadAllBytes((Resolve-Path "$BuildDirectory/Release/odbcpp.dll"))
$offset = [BitConverter]::ToInt32($bytes,60)
# Change AMD64 to I386; it must fail before staging dependencies or building MSI.
$bytes[$offset+4] = 0x4c; $bytes[$offset+5] = 0x01
[IO.File]::WriteAllBytes("$root/Release/odbcpp.dll",$bytes)
$rejected = $false
try { & "$PSScriptRoot/build.ps1" -BuildDirectory $root -OutputDirectory "$root/out" }
catch { if ($_.Exception.Message -notmatch 'requires x64 PE') { throw }; $rejected = $true }
if (!$rejected) { throw 'Non-x64 input was accepted' }
Write-Host 'Package input architecture rejection passed.'

[IO.File]::WriteAllBytes("$root/Release/odbcpp.dll",[byte[]]@(77,90,0))
$rejected = $false
try { & "$PSScriptRoot/build.ps1" -BuildDirectory $root -OutputDirectory "$root/truncated" }
catch { if ($_.Exception.Message -notmatch 'Invalid PE file') { throw }; $rejected = $true }
if (!$rejected) { throw 'Truncated PE input was accepted' }
Write-Host 'Package truncated-input rejection passed.'
