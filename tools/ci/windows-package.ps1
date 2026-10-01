param([string]$Artifacts = 'package-artifacts')
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$artifactsPath = (Resolve-Path $Artifacts).Path
$logs = Join-Path $env:RUNNER_TEMP 'package-logs'
New-Item $logs -ItemType Directory -Force | Out-Null
$driverKey = 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\ODBCPP PostgreSQL'
$ownerKey = 'HKLM:\SOFTWARE\ODBCPP\PostgreSQL'
$driversKey = 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers'
$install = [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'ODBCPP/PostgreSQL'))
function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Msi([string]$Version, [string]$Action, [string]$Label, [int]$Expected = 0) {
    $path = Join-Path $artifactsPath "odbcpp-postgresql-$Version-x64.msi"
    $log = Join-Path $logs "$Label.log"
    $process = Start-Process msiexec.exe -ArgumentList @($Action, "`"$path`"", '/qn', '/norestart', '/l*v', "`"$log`"") -Wait -PassThru
    if ($process.ExitCode -ne $Expected) {
        Get-Content $log -Tail 70
        throw "$Label returned $($process.ExitCode), expected $Expected"
    }
}
Assert (!(Test-Path $driverKey)) 'Fresh runner unexpectedly contains the driver registration'
Assert (!(Test-Path $install)) 'Fresh runner unexpectedly contains the install directory'
# Reject collisions instead of overwriting a pre-existing unrelated registration.
New-Item $driverKey -Force | Out-Null
New-ItemProperty $driverKey -Name Driver -Value 'C:\foreign\driver.dll' -Force | Out-Null
Msi '1.0.0' '/i' 'foreign-registration' 1603
Assert ((Get-ItemProperty $driverKey).Driver -eq 'C:\foreign\driver.dll') 'Foreign registration changed'
Remove-Item $driverKey
Msi '1.0.0' '/i' 'install'
Assert ((Get-ItemProperty $ownerKey).Version -eq '1.0.0') 'Initial version missing'
Assert ((Get-ItemProperty $driverKey).Driver -eq "$install\odbcpp.dll") 'Driver path mismatch'
Assert ((Get-ItemProperty $driverKey).Setup -eq "$install\odbcpp_setup.dll") 'Setup path mismatch'
$inventory = Get-Content "$install/inventory.json" -Raw | ConvertFrom-Json
foreach ($file in $inventory.files) {
    Assert ((Get-FileHash (Join-Path $install $file.file) -Algorithm SHA256).Hash -eq $file.sha256) 'Installed runtime hash differs from inventory'
}
$cryptoEvidence = $inventory.cryptoManifest
Assert ($cryptoEvidence.file -eq 'odbcpp-crypto-manifest.json') 'Crypto manifest inventory entry is missing'
$installedManifest = Join-Path $install $cryptoEvidence.file
Assert ((Get-FileHash $installedManifest -Algorithm SHA256).Hash -eq $cryptoEvidence.sha256) 'Installed crypto manifest hash differs from inventory'
$cryptoManifest = Get-Content $installedManifest -Raw | ConvertFrom-Json
Assert ($cryptoManifest.provider -eq 'OPENSSL') 'Installed crypto provider differs from package profile'
Assert ($cryptoManifest.requestedLinkage -eq 'BUNDLED_SHARED') 'Installed crypto linkage differs from package profile'
Assert ($cryptoManifest.configurationEvidenceOnly -eq $true -and
        $cryptoManifest.actualArtifactLinkageVerified -eq $false -and
        $cryptoManifest.fipsClaimed -eq $false) 'Installed crypto manifest makes an unsupported claim'
$artifactEvidenceEntry = $inventory.cryptoArtifactEvidence
Assert ($artifactEvidenceEntry.file -eq 'odbcpp-crypto-artifact-evidence.json') 'Crypto artifact evidence inventory entry is missing'
$installedArtifactEvidence = Join-Path $install $artifactEvidenceEntry.file
Assert ((Get-FileHash $installedArtifactEvidence -Algorithm SHA256).Hash -eq $artifactEvidenceEntry.sha256) 'Installed crypto artifact evidence hash differs from inventory'
$artifactEvidence = Get-Content $installedArtifactEvidence -Raw | ConvertFrom-Json
Assert ($artifactEvidence.schemaVersion -eq 2 -and $artifactEvidence.provider -eq 'OPENSSL' -and
        $artifactEvidence.requestedLinkage -eq 'BUNDLED_SHARED') 'Installed crypto artifact evidence profile is invalid'
Assert ($artifactEvidence.dependencyFormVerified -eq $true -and
        $artifactEvidence.dependencyOriginVerified -eq $false -and
        $artifactEvidence.configuredLinkInputsVerified -eq $false -and
        $artifactEvidence.qualificationClaimed -eq $false) 'Installed crypto artifact evidence makes an unsupported claim'
Assert ((Get-FileHash "$install/odbcpp.dll" -Algorithm SHA256).Hash -eq $artifactEvidence.artifactSha256) 'Installed artifact evidence does not describe the installed driver'
$cryptoDependencies = @($inventory.cryptoDependencies)
Assert ($cryptoDependencies.Count -eq 2) 'Package inventory must contain exactly two crypto dependencies'
$sslDependency = @($cryptoDependencies | Where-Object role -eq 'ssl')
$cryptoDependency = @($cryptoDependencies | Where-Object role -eq 'crypto')
Assert ($sslDependency.Count -eq 1 -and $cryptoDependency.Count -eq 1) 'Package inventory crypto dependency roles are invalid'
foreach ($dependency in $cryptoDependencies) {
    Assert ([IO.Path]::GetFileName([string]$dependency.file) -ceq [string]$dependency.file) 'Package inventory contains a path-bearing crypto dependency'
    $import = @($artifactEvidence.runtimeDependencies | Where-Object {
        $_.role -eq $dependency.role -and $_.file -ceq $dependency.file
    })
    Assert ($import.Count -eq 1) 'Package crypto inventory differs from the inspected PE imports'
    Assert ((Get-FileHash (Join-Path $install $dependency.file) -Algorithm SHA256).Hash -eq $dependency.sha256) 'Installed crypto runtime hash differs from crypto inventory'
}
# Remove every development dependency directory from the acceptance process PATH.
$hostile = Join-Path $env:RUNNER_TEMP 'hostile-crypto-path'
$validFallback = Join-Path $env:RUNNER_TEMP 'valid-crypto-path-fallback'
New-Item $hostile,$validFallback -ItemType Directory -Force | Out-Null
foreach ($dependency in $cryptoDependencies) {
    Set-Content (Join-Path $hostile $dependency.file) 'not a PE image'
    Copy-Item (Join-Path $install $dependency.file) $validFallback
}
$basePath = "$env:SystemRoot/System32;$env:SystemRoot;${env:ProgramFiles}/PowerShell/7"
$env:PATH = "$hostile;$basePath"
function Invoke-PackageLoad {
    param([string]$Preload = '', [int]$ExpectedExit = 0)
    $previousNativePreference = $PSNativeCommandUseErrorActionPreference
    $PSNativeCommandUseErrorActionPreference = $false
    try {
        $arguments = @($install, $sslDependency[0].file, $cryptoDependency[0].file)
        if ($Preload) { $arguments += $Preload }
        $lines = @(& "$artifactsPath/it_package_load.exe" @arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally { $PSNativeCommandUseErrorActionPreference = $previousNativePreference }
    if ($exitCode -ne $ExpectedExit) { throw "Package loader returned $exitCode, expected ${ExpectedExit}: $($lines -join [Environment]::NewLine)" }
    return $lines
}
# A missing app-local runtime must fail even when PATH contains a valid copy.
$missingRuntime = Join-Path $install $cryptoDependency[0].file
$heldRuntime = "$missingRuntime.odbcpp-held"
Move-Item $missingRuntime $heldRuntime
try {
    $env:PATH = "$validFallback;$basePath"
    $previousNativePreference = $PSNativeCommandUseErrorActionPreference
    $PSNativeCommandUseErrorActionPreference = $false
    $null = & "$artifactsPath/it_package_load.exe" $install $sslDependency[0].file $cryptoDependency[0].file 2>&1
    $missingExit = $LASTEXITCODE
    Assert ($missingExit -ne 0) 'Package loader fell back to a valid PATH copy for a missing app-local crypto runtime'
} finally {
    $PSNativeCommandUseErrorActionPreference = $previousNativePreference
    $env:PATH = "$hostile;$basePath"
    Move-Item $heldRuntime $missingRuntime
}
$loaderOutput = @(Invoke-PackageLoad)
# Fresh processes distinguish supported same-file preloading from a host copy
# with identical bytes but a different origin. This is a detector, not a driver
# mitigation: the foreign case loads the driver before inspecting its bound IAT.
$samePreloadOutput = @(Invoke-PackageLoad -Preload $install)
Assert (@($samePreloadOutput | Where-Object { $_.ToString() -eq 'PRELOAD_READY' }).Count -eq 1) 'Same-package preload was not exercised'
$samePreloadOutput | Set-Content (Join-Path $logs 'crypto-same-package-preload.log') -Encoding utf8
$foreignPreloadOutput = @(Invoke-PackageLoad -Preload $validFallback -ExpectedExit 3)
Assert (@($foreignPreloadOutput | Where-Object { $_.ToString() -eq 'PRELOAD_READY' }).Count -eq 1) 'Foreign preload did not complete before driver load'
Assert (@($foreignPreloadOutput | Where-Object { $_.ToString() -eq 'FOREIGN_PRELOAD_BOUND_TO_DRIVER' }).Count -eq 1) 'Foreign preload failure did not identify a bound driver import collision'
$foreignPreloadOutput | Set-Content (Join-Path $logs 'crypto-foreign-package-preload.log') -Encoding utf8
foreach ($dependency in $cryptoDependencies) {
    Assert ((Get-FileHash (Join-Path $validFallback $dependency.file) -Algorithm SHA256).Hash -eq $dependency.sha256) 'Foreign preload fixture differs from installed provider bytes'
}
$missingPreload = Join-Path $env:RUNNER_TEMP 'missing-crypto-preload'
New-Item $missingPreload -ItemType Directory -Force | Out-Null
$missingPreloadOutput = @(Invoke-PackageLoad -Preload $missingPreload -ExpectedExit 2)
Assert (@($missingPreloadOutput | Where-Object { $_.ToString() -eq 'PRELOAD_READY' }).Count -eq 0) 'Missing-provider canary silently degraded to ordinary driver loading'
$missingPreloadOutput | Set-Content (Join-Path $logs 'crypto-missing-preload.log') -Encoding utf8
$loadedModules = @()
foreach ($dependency in $cryptoDependencies) {
    $prefix = "MODULE|$($dependency.file)|"
    $match = @($loaderOutput | Where-Object { $_.ToString().StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
    Assert ($match.Count -eq 1) "Loader evidence is missing $($dependency.file)"
    $loadedPath = $match[0].ToString().Substring($prefix.Length)
    Assert ([IO.Path]::GetFullPath($loadedPath) -ieq [IO.Path]::GetFullPath((Join-Path $install $dependency.file))) 'Loaded crypto runtime path differs from the installed package path'
    $loadedHash = (Get-FileHash $loadedPath -Algorithm SHA256).Hash
    Assert ($loadedHash -eq $dependency.sha256) 'Loaded crypto runtime hash differs from the package inventory'
    $loadedModules += @{
        role = $dependency.role; file = $dependency.file; path = $loadedPath
        expectedSha256 = $dependency.sha256; loadedSha256 = $loadedHash; hashMatched = $true
    }
}
$versionLines = @($loaderOutput | Where-Object { $_.ToString().StartsWith('VERSION|') })
Assert ($versionLines.Count -eq 1) 'Loader evidence is missing the OpenSSL runtime version'
$runtimeVersion = $versionLines[0].ToString().Substring('VERSION|'.Length)
$versionMatch = [regex]::Match($runtimeVersion, '^OpenSSL\s+(\d+\.\d+\.\d+)(?:\s|$)')
Assert ($versionMatch.Success) 'Loaded crypto runtime reported an unexpected provider/version string'
Assert ($versionMatch.Groups[1].Value -eq $cryptoManifest.compileVersion) 'Loaded OpenSSL version differs from the configured build version'
@{
    schemaVersion = 1
    driverSha256 = (Get-FileHash "$install/odbcpp.dll" -Algorithm SHA256).Hash
    artifactEvidenceSha256 = (Get-FileHash $installedArtifactEvidence -Algorithm SHA256).Hash
    importedDependencies = @($artifactEvidence.runtimeDependencies)
    loadedModules = $loadedModules
    compileVersion = $cryptoManifest.compileVersion
    runtimeVersion = $runtimeVersion
    compileRuntimeVersionMatched = $true
    loaderSearchFlags = @('LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR','LOAD_LIBRARY_SEARCH_SYSTEM32')
    hostilePathOverrideRejected = $true
    missingLocalValidPathFallbackRejected = $true
    packagedLoaderOriginVerified = $true
    originResolvedFromDriverImportAddresses = $true
    samePackagePreloadVerified = $true
    foreignSameBasenamePreloadCollisionDetected = $true
    missingPreloadCanaryVerified = $true
    foreignPreloadPreventedByDriver = $false
    preloadInputs = @($cryptoDependencies | ForEach-Object {
        @{ file = $_.file; installedSha256 = $_.sha256
           foreignCopySha256 = (Get-FileHash (Join-Path $validFallback $_.file) -Algorithm SHA256).Hash }
    })
    preloadReports = @(@('crypto-same-package-preload.log','crypto-foreign-package-preload.log','crypto-missing-preload.log') | ForEach-Object {
        @{ file = $_; sha256 = (Get-FileHash (Join-Path $logs $_) -Algorithm SHA256).Hash }
    })
    preloadedModuleCoexistenceVerified = $false
    qualificationClaimed = $false
} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $logs 'crypto-loader-evidence.json') -Encoding utf8
& "$PSScriptRoot/windows-postgres.ps1" -Action Start -UseInstalledDriver
try {
    # The fixture configures DSNs only; MSI supplies driver/setup registration.
    & "$artifactsPath/it_driver_manager.exe"
    & "$artifactsPath/it_setup.exe"
    $administratorFailure = $null
    try {
        & "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/windows-administrator.ps1"
    } catch { $administratorFailure = $_; Write-Warning 'Administrator UI failed; continuing independent lifecycle checks before reporting failure.' }
    New-ItemProperty $driverKey -Name UnrelatedValue -Value 'keep' -Force | Out-Null
    New-ItemProperty $driversKey -Name 'ODBCPP unrelated fixture' -Value Installed -Force | Out-Null
    Msi '1.0.2' '/i' 'failed-upgrade-rollback' 1603
    Assert (Select-String -Path (Join-Path $logs 'failed-upgrade-rollback.log') -SimpleMatch 'Intentional rollback acceptance failure.' -Quiet) 'Upgrade failed before the rollback injection point'
    Assert ((Get-ItemProperty $ownerKey).Version -eq '1.0.0') 'Failed upgrade did not restore previous version'
    $null = Invoke-PackageLoad
    & "$artifactsPath/it_driver_manager.exe"
    Msi '1.0.1' '/i' 'upgrade'
    Assert ((Get-ItemProperty $ownerKey).Version -eq '1.0.1') 'Upgrade version missing'
    & "$artifactsPath/it_driver_manager.exe"
    Msi '1.0.0' '/i' 'downgrade-rejected' 1603
    # An external repoint must block uninstall instead of deleting foreign ownership.
    Set-ItemProperty $driverKey -Name Driver -Value 'C:\foreign\replacement.dll'
    Msi '1.0.1' '/x' 'changed-registration-rejected' 1603
    Assert ((Get-ItemProperty $driverKey).Driver -eq 'C:\foreign\replacement.dll') 'Foreign replacement changed'
    Set-ItemProperty $driverKey -Name Driver -Value "$install\odbcpp.dll"
    Msi '1.0.1' '/x' 'uninstall'
    Assert (!(Test-Path "$install/odbcpp.dll")) 'Owned DLL remains after uninstall'
    Assert (!$((Get-ItemProperty $driverKey).PSObject.Properties['Driver'])) 'Driver registration remains'
    Assert ((Get-ItemProperty $driverKey).UnrelatedValue -eq 'keep') 'Unrelated registration value removed'
    Assert ((Get-ItemProperty $driversKey).'ODBCPP unrelated fixture' -eq 'Installed') 'Other driver listing removed'
    Assert (Test-Path 'HKLM:\SOFTWARE\ODBC\ODBC.INI\RedshiftProd') 'Uninstall deleted a DSN'
    Assert (!(Get-ItemProperty $ownerKey -Name OwnedDriver -ErrorAction SilentlyContinue)) 'Package ownership marker remains'
    if ($administratorFailure) { throw $administratorFailure }
    Write-Host 'Fresh-runner MSI install/configure/connect/rollback/upgrade/uninstall acceptance passed.'
} finally {
    & "$PSScriptRoot/windows-postgres.ps1" -Action Stop
}
