# Existing disposable Windows CI token owns installation; this never requests elevation.
param([string]$Artifacts = 'package-artifacts', [switch]$VerifyCleanup)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
if ($VerifyCleanup) {
    # Read-only mode grants no installer action and needs no administrator token.
    $evidence = Join-Path $env:RUNNER_TEMP 'dual-product-package-evidence'
    $summaryPath = Join-Path $evidence 'lifecycle/summary.json'
    if (!(Test-Path -LiteralPath $summaryPath)) { throw 'Legacy package cases unrun: combined installer cleanup is unavailable.' }
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    $cleanup = @($summary.cleanup)
    $names = @($cleanup | ForEach-Object product | Sort-Object) -join ','
    if ($summary.result -notin @('PASS','FAIL') -or
        $cleanup.Count -ne 2 -or $names -cne 'PostgreSQL,Redshift' -or
        @($cleanup | Where-Object { !$_.owned_installation_absent -or $_.error }).Count -ne 0) {
        throw 'Legacy package cases unrun: combined owned MSI cleanup is not verified.'
    }
    Write-Host 'Both owned installations are absent; independent legacy package cases may run.'
    return
}
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Unattended package qualification requires an already elevated disposable CI session; no elevation was requested.'
}
$source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$artifactsPath = (Resolve-Path -LiteralPath $Artifacts).Path
$evidence = Join-Path $env:RUNNER_TEMP 'dual-product-package-evidence'
if (Test-Path -LiteralPath $evidence) { throw 'Preserve previous package evidence' }
New-Item -ItemType Directory -Path $evidence | Out-Null
$build = Join-Path $source 'build-package-redshift'
$output = Join-Path $build 'package'
$setupPg = Join-Path $source 'build-package-setup-postgresql'
$setupRs = Join-Path $source 'build-package-setup-redshift'
foreach ($path in @($build,$setupPg,$setupRs)) {
    if (Test-Path -LiteralPath $path) { throw 'Use fresh owned package build graphs' }
}
$head = (& git -C $source rev-parse HEAD).Trim()
if ($head -cne $env:GITHUB_SHA) { throw 'Package source differs from dispatched checkout' }
@{ source_head=$head; source_tree=(& git -C $source rev-parse 'HEAD^{tree}').Trim(); route='Existing elevated disposable CI job'; database_operations=0; cloud_operations=0 } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'source.json') -Encoding UTF8

& cmake -S $source -B $build -A x64 -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF `
    -DTARGET_DATABASE=REDSHIFT -DODBCPP_CRYPTO_PROVIDER=OPENSSL `
    -DODBCPP_CRYPTO_LINKAGE=BUNDLED_SHARED '-DODBCPP_CRYPTO_ROOT=C:/Program Files/OpenSSL' `
    -DODBCPP_CRYPTO_PROFILE_LIVE_TESTS=OFF -DODBCPP_CRYPTO_TLS_PROOF=OFF `
    -DODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=2 -DODBCPP_EXPECT_DM_SQLWCHAR_SIZE=2
& cmake --build $build --config Release --target odbcpp_driver odbcpp_setup --parallel 2
foreach ($version in @('1.0.0','1.0.1','1.0.2')) {
    & (Join-Path $source 'packaging/windows/build.ps1') -BuildDirectory $build -Version $version -FailureFixture:($version -eq '1.0.2')
}
foreach ($entry in @(@{database='POSTGRESQL';build=$setupPg},@{database='REDSHIFT';build=$setupRs})) {
    & cmake -S (Join-Path $source 'tests/windows/installed_setup') -B $entry.build -A x64 ('-DTARGET_DATABASE='+$entry.database)
    & cmake --build $entry.build --config Release --parallel 2
}
& (Join-Path $source 'packaging/windows/test-product-lifecycle.ps1') `
    -PostgreSqlPackage (Join-Path $artifactsPath 'odbcpp-postgresql-1.0.0-x64.msi') `
    -RedshiftPackage (Join-Path $output 'odbcpp-redshift-1.0.0-x64.msi') `
    -RedshiftUpgrade (Join-Path $output 'odbcpp-redshift-1.0.1-x64.msi') `
    -RedshiftRollbackFixture (Join-Path $output 'odbcpp-redshift-1.0.2-x64.msi') `
    -LoaderExecutable (Join-Path $artifactsPath 'it_package_load.exe') `
    -PostgreSqlSetupBuild $setupPg -RedshiftSetupBuild $setupRs `
    -CTestExecutable (Join-Path (Split-Path (Get-Command cmake).Source -Parent) 'ctest.exe') `
    -EvidenceDirectory (Join-Path $evidence 'lifecycle')
Write-Host 'Both installed product setup suites and MSI coexistence/rollback/upgrade/removal passed.'
