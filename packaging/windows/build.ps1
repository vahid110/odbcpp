param(
    [string]$BuildDirectory = 'build-windows',
    [string]$OutputDirectory = 'build-windows/package',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version = '1.0.0',
    [string]$OpenSslRoot = 'C:/Program Files/OpenSSL',
    [switch]$FailureFixture
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
function Assert-X64([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { throw "Invalid PE file: $Path" }
    $offset = [BitConverter]::ToInt32($bytes, 60)
    if ($offset -lt 0 -or $offset + 6 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $offset) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $offset + 4) -ne 0x8664) { throw "Package requires x64 PE files: $Path" }
}
$build = (Resolve-Path $BuildDirectory).Path
if (!(Select-String -Path "$build/CMakeCache.txt" -Pattern '^TARGET_DATABASE:STRING=POSTGRESQL$' -Quiet)) {
    throw 'The beta package requires a PostgreSQL build'
}
New-Item $OutputDirectory -ItemType Directory -Force | Out-Null
$output = (Resolve-Path $OutputDirectory).Path
$stage = Join-Path $output "payload-$Version"
if (Test-Path $stage) { throw "Staging directory already exists: $stage" }
New-Item $stage -ItemType Directory | Out-Null
foreach ($file in @('odbcpp.dll','odbcpp_setup.dll')) {
    $source = Join-Path $build "Release/$file"
    Assert-X64 $source
    Copy-Item $source $stage
}
# Only the runtime libraries imported by our OpenSSL 3 build; never copy tools,
# private keys, configuration files or the PostgreSQL runner installation.
foreach ($name in @('libssl-3-x64.dll','libcrypto-3-x64.dll')) {
    $opensslCandidates = @(Get-ChildItem $OpenSslRoot -Filter $name -Recurse -File)
    if ($opensslCandidates.Count -ne 1) { throw "Expected one OpenSSL runtime $name, found $($opensslCandidates.Count)" }
    Assert-X64 $opensslCandidates[0].FullName
    Copy-Item $opensslCandidates[0].FullName $stage
}
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = @(Get-Item "$vs/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT" | Sort-Object FullName -Descending)[0]
if (!$crt) { throw 'MSVC x64 redistributable directory not found' }
Get-ChildItem $crt.FullName -Filter *.dll | ForEach-Object { Assert-X64 $_.FullName; Copy-Item $_.FullName $stage }
$license = @(Get-ChildItem $OpenSslRoot -Recurse -File | Where-Object { $_.Name -match '^(LICENSE|LICENSE\.txt|LICENSE\.md|LICENSE\.html)$' })
if (!$license) { throw 'OpenSSL license file is required for packaging' }
Copy-Item $license[0].FullName (Join-Path $stage 'OpenSSL-LICENSE.txt')
Copy-Item "$build/_deps/spdlog-src/LICENSE" (Join-Path $stage 'spdlog-LICENSE.txt')
Copy-Item "$build/_deps/spdlog-src/include/spdlog/fmt/bundled/fmt.license.rst" (Join-Path $stage 'fmt-LICENSE.txt')
Copy-Item "$PSScriptRoot/README.md" (Join-Path $stage 'README.txt')
$inventory = @(Get-ChildItem $stage -Filter *.dll | Sort-Object Name | ForEach-Object {
    @{ file = $_.Name; version = $_.VersionInfo.FileVersion; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
})
@{
    packageVersion = $Version; architecture = 'x64'; wixVersion = '6.0.2'
    sourceRevision = (& git rev-parse HEAD); files = $inventory
    licenses = @{
        OpenSSL = 'OpenSSL-LICENSE.txt'; spdlog = 'spdlog-LICENSE.txt'; fmt = 'fmt-LICENSE.txt'
        MSVC = 'https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution'
        ODBCPP = 'Project license not yet declared; this is an internal beta validation artifact.'
    }
} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $stage 'inventory.json') -Encoding utf8
$toolDirectory = Join-Path $output 'tools'
if (!(Test-Path "$toolDirectory/wix.exe")) { dotnet tool install wix --version 6.0.2 --tool-path $toolDirectory | Out-Null }
$wix = Join-Path $toolDirectory 'wix.exe'
if ((& $wix --version) -notmatch '^6\.0\.2') { throw 'Unexpected WiX tool version' }
# Sorted payload and stable component IDs keep upgrades reproducible from the same inputs.
$components = [Text.StringBuilder]::new()
[void]$components.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"><Fragment><ComponentGroup Id="Payload" Directory="INSTALLFOLDER">')
foreach ($file in (Get-ChildItem $stage -File | Sort-Object Name)) {
    $id = 'Payload_' + ($file.Name -replace '[^a-zA-Z0-9_]', '_')
    $source = [Security.SecurityElement]::Escape($file.FullName)
    [void]$components.AppendLine("<Component Id=`"$id`" Guid=`"*`" Bitness=`"always64`"><File Source=`"$source`" KeyPath=`"yes`" /></Component>")
}
[void]$components.AppendLine('</ComponentGroup></Fragment></Wix>')
$payload = Join-Path $output "payload-$Version.wxs"
[IO.File]::WriteAllText($payload, $components.ToString())
$msi = Join-Path $output "odbcpp-postgresql-$Version-x64.msi"
$failure = if ($FailureFixture) { '1' } else { '0' }
& $wix build -arch x64 -d "Version=$Version" -d "FailureFixture=$failure" "$PSScriptRoot/Product.wxs" $payload -o $msi
(Get-FileHash $msi -Algorithm SHA256).Hash + '  ' + (Split-Path $msi -Leaf) | Set-Content "$msi.sha256"
Write-Host "Built x64 MSI $Version with $($inventory.Count) runtime libraries."
