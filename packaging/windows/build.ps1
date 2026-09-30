param(
    [string]$BuildDirectory = 'build-windows',
    [string]$OutputDirectory = 'build-windows/package',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version = '1.0.0',
    [string]$OpenSslRoot = 'C:/Program Files/OpenSSL',
    [switch]$FailureFixture
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
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
$manifestPath = Join-Path $build 'odbcpp-crypto-manifest.json'
if (!(Test-Path $manifestPath)) { throw 'The generated crypto manifest is required for packaging' }
try { $cryptoManifest = Get-Content $manifestPath -Raw | ConvertFrom-Json }
catch { throw "The generated crypto manifest is invalid JSON: $_" }
Assert ($cryptoManifest.schemaVersion -eq 1) 'Unsupported crypto manifest schema'
Assert ($cryptoManifest.provider -eq 'OPENSSL') 'The Windows beta package requires the OPENSSL provider'
Assert ($cryptoManifest.requestedLinkage -eq 'BUNDLED_SHARED') 'The Windows beta package requires BUNDLED_SHARED crypto'
Assert ($cryptoManifest.configurationEvidenceOnly -eq $true) 'Crypto manifest must identify configure-time evidence'
Assert ($cryptoManifest.actualArtifactLinkageVerified -eq $false) 'Packaging cannot accept a pre-claimed crypto artifact'
Assert ($cryptoManifest.fipsClaimed -eq $false) 'The Windows beta package does not claim FIPS'
$trimSeparators = [char[]]@('\','/')
function Assert-NoReparsePoints([string]$Path) {
    $current = [IO.Path]::GetFullPath((Resolve-Path $Path).Path)
    while ($current) {
        $item = Get-Item -LiteralPath $current -Force
        Assert (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Controlled crypto path contains a reparse point: $current"
        $parent = [IO.Directory]::GetParent($current)
        if (!$parent) { break }
        $current = $parent.FullName
    }
}
Assert-NoReparsePoints $OpenSslRoot
$expectedRoot = [IO.Path]::GetFullPath($OpenSslRoot).TrimEnd($trimSeparators)
$manifestRoot = [IO.Path]::GetFullPath([string]$cryptoManifest.dependencyRoot).TrimEnd($trimSeparators)
Assert ($expectedRoot -ieq $manifestRoot) 'Crypto manifest dependency root does not match the packaging input'
Assert-NoReparsePoints ([string]$cryptoManifest.dependencyRoot)
$driverSource = Join-Path $build 'Release/odbcpp.dll'
Assert-X64 $driverSource
$inspectorMatch = Select-String -Path "$build/CMakeCache.txt" -Pattern '^ODBCPP_CRYPTO_INSPECTOR:FILEPATH=(.+)$'
Assert ($inspectorMatch.Count -eq 1) 'The configured PE dependency inspector is required for packaging'
$inspector = $inspectorMatch.Matches[0].Groups[1].Value
Assert (Test-Path $inspector) 'The configured PE dependency inspector is missing'
$artifactEvidencePath = Join-Path $build 'Release/odbcpp-crypto-artifact-evidence.json'
& cmake "-DARTIFACT=$driverSource" '-DEXPECTED_PROVIDER=OPENSSL' '-DEXPECTED_LINKAGE=BUNDLED_SHARED' `
    '-DPLATFORM=Windows' "-DINSPECTOR=$inspector" "-DCONFIG_MANIFEST=$manifestPath" `
    "-DEVIDENCE=$artifactEvidencePath" -P (Join-Path $PSScriptRoot '../../cmake/InspectCryptoArtifact.cmake')
try { $artifactEvidence = Get-Content $artifactEvidencePath -Raw | ConvertFrom-Json }
catch { throw "The crypto artifact evidence is invalid JSON: $_" }
Assert ($artifactEvidence.schemaVersion -eq 2) 'Unsupported crypto artifact evidence schema'
Assert ($artifactEvidence.provider -eq 'OPENSSL' -and $artifactEvidence.requestedLinkage -eq 'BUNDLED_SHARED') 'Crypto artifact evidence profile differs from package profile'
Assert ($artifactEvidence.dependencyFormVerified -eq $true) 'Crypto artifact dependency form is unverified'
Assert ($artifactEvidence.dependencyOriginVerified -eq $false -and $artifactEvidence.configuredLinkInputsVerified -eq $false) 'Windows artifact evidence makes an unsupported origin claim'
Assert ($artifactEvidence.qualificationClaimed -eq $false) 'Windows artifact evidence cannot pre-claim qualification'
Assert ($artifactEvidence.artifactSha256 -eq (Get-FileHash $driverSource -Algorithm SHA256).Hash) 'Crypto artifact evidence is not bound to the packaged driver'
$runtimeDependencies = @($artifactEvidence.runtimeDependencies)
Assert ($runtimeDependencies.Count -eq 2) 'Crypto artifact evidence must identify exactly two runtime dependencies'
Assert (@($runtimeDependencies | Where-Object role -eq 'ssl').Count -eq 1) 'Crypto artifact evidence must identify one SSL runtime'
Assert (@($runtimeDependencies | Where-Object role -eq 'crypto').Count -eq 1) 'Crypto artifact evidence must identify one Crypto runtime'
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
function Assert-InsideRoot([string]$Path, [string]$Root) {
    Assert-NoReparsePoints $Path
    $canonicalPath = [IO.Path]::GetFullPath((Resolve-Path $Path).Path)
    $canonicalRoot = [IO.Path]::GetFullPath((Resolve-Path $Root).Path).TrimEnd($trimSeparators) + [IO.Path]::DirectorySeparatorChar
    Assert ($canonicalPath.StartsWith($canonicalRoot, [StringComparison]::OrdinalIgnoreCase)) "Crypto runtime escaped the controlled dependency root: $canonicalPath"
    return $canonicalPath
}
$cryptoDependencies = @()
foreach ($dependency in $runtimeDependencies) {
    $name = [string]$dependency.file
    Assert ([IO.Path]::GetFileName($name) -ceq $name) 'Crypto artifact evidence contains a path-bearing runtime dependency'
    $runtime = Join-Path $OpenSslRoot "bin/$name"
    if (!(Test-Path $runtime)) { $runtime = Join-Path $OpenSslRoot $name }
    if (!(Test-Path $runtime)) { throw "OpenSSL runtime directory is missing $name" }
    $runtime = Assert-InsideRoot $runtime $OpenSslRoot
    Assert-X64 $runtime
    Copy-Item $runtime $stage
    $stagedRuntime = Join-Path $stage $name
    $runtimeHash = (Get-FileHash $runtime -Algorithm SHA256).Hash
    Assert ((Get-FileHash $stagedRuntime -Algorithm SHA256).Hash -eq $runtimeHash) 'Staged crypto runtime hash differs from its controlled source'
    $cryptoDependencies += @{
        role = [string]$dependency.role; file = $name; source = $runtime
        version = (Get-Item $runtime).VersionInfo.FileVersion; sha256 = $runtimeHash
    }
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
Copy-Item $manifestPath (Join-Path $stage 'odbcpp-crypto-manifest.json')
Copy-Item $artifactEvidencePath (Join-Path $stage 'odbcpp-crypto-artifact-evidence.json')
function Macro([string]$Path, [string]$Name) {
    $match = [regex]::Match((Get-Content $Path -Raw), '(?m)^#define ' + [regex]::Escape($Name) + '\s+(\d+)\s*$')
    if (!$match.Success) { throw "Missing dependency version macro $Name" }
    return [int]$match.Groups[1].Value
}
$spdlogVersion = (@('MAJOR','MINOR','PATCH') | ForEach-Object { Macro "$build/_deps/spdlog-src/include/spdlog/version.h" "SPDLOG_VER_$_" }) -join '.'
$fmtNumber = Macro "$build/_deps/spdlog-src/include/spdlog/fmt/bundled/base.h" 'FMT_VERSION'
$fmtVersion = '{0}.{1}.{2}' -f [math]::Floor($fmtNumber / 10000), [math]::Floor(($fmtNumber % 10000) / 100), ($fmtNumber % 100)
$inventory = @(Get-ChildItem $stage -Filter *.dll | Sort-Object Name | ForEach-Object {
    @{ file = $_.Name; version = $_.VersionInfo.FileVersion; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
})
$cryptoEvidence = Get-Item (Join-Path $stage 'odbcpp-crypto-manifest.json')
$artifactEvidenceFile = Get-Item (Join-Path $stage 'odbcpp-crypto-artifact-evidence.json')
@{
    packageVersion = $Version; architecture = 'x64'; wixVersion = '6.0.2'
    sourceRevision = (& git rev-parse HEAD); files = $inventory
    headerOnlyDependencies = @{ spdlog = $spdlogVersion; fmt = $fmtVersion }
    runnerImageVersion = $env:ImageVersion
    cryptoManifest = @{
        file = $cryptoEvidence.Name
        sha256 = (Get-FileHash $cryptoEvidence.FullName -Algorithm SHA256).Hash
        provider = $cryptoManifest.provider
        requestedLinkage = $cryptoManifest.requestedLinkage
    }
    cryptoArtifactEvidence = @{
        file = $artifactEvidenceFile.Name
        sha256 = (Get-FileHash $artifactEvidenceFile.FullName -Algorithm SHA256).Hash
        provider = $artifactEvidence.provider
        requestedLinkage = $artifactEvidence.requestedLinkage
        artifactSha256 = $artifactEvidence.artifactSha256
    }
    cryptoDependencies = $cryptoDependencies
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
