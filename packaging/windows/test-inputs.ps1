# Run before packaging: negative PE checks and schema safeguards need no installer.
param(
    [string]$BuildDirectory = 'build-windows',
    [string]$OpenSslRoot = 'C:/Program Files/OpenSSL'
)
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:RUNNER_TEMP 'odbcpp-invalid-package'
New-Item "$root/Release" -ItemType Directory -Force | Out-Null
Copy-Item "$BuildDirectory/CMakeCache.txt" $root
Copy-Item "$BuildDirectory/odbcpp-crypto-manifest.json" $root
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

Copy-Item "$BuildDirectory/Release/odbcpp.dll","$BuildDirectory/Release/odbcpp_setup.dll" "$root/Release" -Force
$manifest = Get-Content "$BuildDirectory/odbcpp-crypto-manifest.json" -Raw | ConvertFrom-Json
$manifest.requestedLinkage = 'SYSTEM_SHARED'
$manifest | ConvertTo-Json -Depth 5 | Set-Content "$root/odbcpp-crypto-manifest.json" -Encoding utf8
$rejected = $false
try { & "$PSScriptRoot/build.ps1" -BuildDirectory $root -OutputDirectory "$root/wrong-profile" }
catch { if ($_.Exception.Message -notmatch 'requires BUNDLED_SHARED crypto') { throw }; $rejected = $true }
if (!$rejected) { throw 'Wrong crypto package profile was accepted' }
Write-Host 'Package crypto-profile rejection passed.'

Copy-Item "$BuildDirectory/odbcpp-crypto-manifest.json" "$root/odbcpp-crypto-manifest.json" -Force
$junction = Join-Path $root 'openssl-junction'
New-Item -ItemType Junction -Path $junction -Target $OpenSslRoot | Out-Null
$manifest = Get-Content "$root/odbcpp-crypto-manifest.json" -Raw | ConvertFrom-Json
$manifest.dependencyRoot = $junction
$manifest | ConvertTo-Json -Depth 5 | Set-Content "$root/odbcpp-crypto-manifest.json" -Encoding utf8
$rejected = $false
try { & "$PSScriptRoot/build.ps1" -BuildDirectory $root -OutputDirectory "$root/reparse-root" -OpenSslRoot $junction }
catch { if ($_.Exception.Message -notmatch 'contains a reparse point') { throw }; $rejected = $true }
if (!$rejected) { throw 'Reparse-point crypto dependency root was accepted' }
Write-Host 'Package crypto reparse-point rejection passed.'
