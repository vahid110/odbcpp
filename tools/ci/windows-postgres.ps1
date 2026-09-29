param([ValidateSet('Start', 'Stop')][string]$Action)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$pgBin = Join-Path $env:PGBIN 'pg_ctl.exe'
$pgData = Join-Path $env:RUNNER_TEMP 'odbcpp-postgres'
if ($Action -eq 'Stop') {
    if (Test-Path (Join-Path $pgData 'postmaster.pid')) {
        & $pgBin -D $pgData -m fast -w stop
    }
    exit 0
}
if (!(Test-Path $pgBin)) { throw 'The runner PostgreSQL installation is missing' }
$passwordFile = Join-Path $env:RUNNER_TEMP 'odbcpp-postgres-password'
Set-Content $passwordFile 'postgres' -Encoding ascii
try {
    & "$env:PGBIN/initdb.exe" -D $pgData -U postgres --auth=scram-sha-256 --pwfile=$passwordFile --no-locale --encoding=UTF8
} finally {
    Remove-Item $passwordFile -ErrorAction SilentlyContinue
}
& $pgBin -D $pgData -l "$env:RUNNER_TEMP/odbcpp-postgres.log" -o '-p 5432 -h 127.0.0.1' -w start
& "$env:PGBIN/psql.exe" -X -v ON_ERROR_STOP=1 -c 'SELECT version()'

$driver = (Resolve-Path 'build-windows/Release/odbcpp.dll').Path
$driverName = 'ODBCPP PostgreSQL'
$driverKey = "HKLM:\SOFTWARE\ODBC\ODBCINST.INI\$driverName"
New-Item $driverKey -Force | Out-Null
New-ItemProperty $driverKey -Name Driver -Value $driver -PropertyType String -Force | Out-Null
$driversKey = 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers'
New-Item $driversKey -Force | Out-Null
New-ItemProperty $driversKey -Name $driverName -Value Installed -PropertyType String -Force | Out-Null
$dsnKey = 'HKLM:\SOFTWARE\ODBC\ODBC.INI\RedshiftProd'
New-Item $dsnKey -Force | Out-Null
New-ItemProperty $dsnKey -Name Driver -Value $driver -PropertyType String -Force | Out-Null
$dsnsKey = 'HKLM:\SOFTWARE\ODBC\ODBC.INI\ODBC Data Sources'
New-Item $dsnsKey -Force | Out-Null
New-ItemProperty $dsnsKey -Name RedshiftProd -Value $driverName -PropertyType String -Force | Out-Null

# The driver currently reads DSN attributes from INI files; Windows Driver
# Manager uses the registry above to locate and load the actual driver DLL.
New-Item $env:ODBCSYSINI -ItemType Directory -Force | Out-Null
@"
[RedshiftProd]
Driver=$driverName
Server=127.0.0.1
Port=5432
Database=postgres
UID=postgres
PWD=postgres
SSL=0
TransportMode=Sync
DeadlineModel=Strict
"@ | Set-Content $env:ODBCINI -Encoding utf8
@"
[$driverName]
Driver=$driver
TransportMode=Sync
DeadlineModel=Strict
"@ | Set-Content $env:ODBCINSTINI -Encoding utf8
