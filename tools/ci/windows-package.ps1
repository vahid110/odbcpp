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
# Remove every development dependency directory from the acceptance process PATH.
$env:PATH = "$env:SystemRoot/System32;$env:SystemRoot;${env:ProgramFiles}/PowerShell/7"
& "$artifactsPath/it_package_load.exe" $install
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
    & "$artifactsPath/it_package_load.exe" $install
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
