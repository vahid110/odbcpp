# Run the real Windows Driver Manager executable as LocalSystem. Its SQLConnectW
# DSN path must read the machine DSN, not the runner account's same-named User DSN.
$ErrorActionPreference = 'Stop'
$taskName = "odbcpp-system-dsn-$env:GITHUB_RUN_ID"
$userDsn = 'HKCU:\SOFTWARE\ODBC\ODBC.INI\RedshiftProd'
if (Test-Path $userDsn) { throw 'Unexpected existing runner User DSN; refusing to replace it' }
$configFile = Join-Path $env:RUNNER_TEMP 'odbcpp-system-dsn.json'
$scriptFile = Join-Path $env:RUNNER_TEMP 'odbcpp-system-dsn-run.ps1'
$resultFile = Join-Path $env:RUNNER_TEMP 'odbcpp-system-dsn.exit'
$logFile = Join-Path $env:RUNNER_TEMP 'odbcpp-system-dsn.log'
@{
    executable = (Resolve-Path 'build-windows/tests/Release/it_driver_manager.exe').Path
    path = $env:PATH
    log = $logFile
    result = $resultFile
    configRoot = (Join-Path $env:RUNNER_TEMP 'odbcpp-no-ini')
} | ConvertTo-Json | Set-Content $configFile -Encoding utf8
@'
param([string]$Configuration)
$ErrorActionPreference = 'Stop'
$config = Get-Content $Configuration -Raw | ConvertFrom-Json
try {
    if ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -ne 'S-1-5-18') {
        throw 'Acceptance process is not LocalSystem'
    }
    $env:PATH = $config.path
    $env:ODBCSYSINI = $config.configRoot
    $env:ODBCINI = Join-Path $config.configRoot 'absent.ini'
    $env:ODBCINSTINI = Join-Path $config.configRoot 'absent-inst.ini'
    & $config.executable *> $config.log
    $code = $LASTEXITCODE
    Set-Content $config.result $code
} catch {
    $_ | Out-String | Set-Content $config.log
    Set-Content $config.result 1
}
'@ | Set-Content $scriptFile -Encoding utf8
try {
    New-Item $userDsn -Force | Out-Null
    foreach ($entry in @{ Server = '127.0.0.1'; Port = '1'; Database = 'wrong-user-dsn' }.GetEnumerator()) {
        New-ItemProperty $userDsn -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
    }
    $action = New-ScheduledTaskAction -Execute (Get-Command pwsh).Source `
        -Argument "-NoLogo -NoProfile -NonInteractive -File `"$scriptFile`" -Configuration `"$configFile`""
    $principal = New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName $taskName
    $deadline = (Get-Date).AddSeconds(90)
    while (!(Test-Path $resultFile) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if (!(Test-Path $resultFile)) { throw 'LocalSystem Driver Manager test timed out' }
    if ([int](Get-Content $resultFile -Raw) -ne 0) {
        Get-Content $logFile
        throw 'LocalSystem registry-only DSN test failed'
    }
    Write-Host 'LocalSystem Driver Manager acceptance passed using registry-only System DSN.'
} finally {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $userDsn -Recurse -ErrorAction SilentlyContinue
}
