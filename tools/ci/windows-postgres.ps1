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
$setup = (Resolve-Path 'build-windows/Release/odbcpp_setup.dll').Path
New-ItemProperty $driverKey -Name Setup -Value $setup -PropertyType String -Force | Out-Null
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

# Native registry attributes only. No INI files may mask a broken registry reader.
$attributes = @{
    Server = '127.0.0.1'; Port = '5432'; Database = 'postgres'
    UID = 'postgres'; PWD = 'postgres'; SSL = '0'
}
foreach ($entry in $attributes.GetEnumerator()) {
    New-ItemProperty $dsnKey -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
}
foreach ($entry in @{ TransportMode = 'Sync'; DeadlineModel = 'Strict' }.GetEnumerator()) {
    New-ItemProperty $driverKey -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
}
foreach ($file in @($env:ODBCINI, $env:ODBCINSTINI)) {
    if (Test-Path $file) { Remove-Item $file }
}
# Conflicting 32-bit System DSN: the x64 driver/DM must use their own view.
$registry32 = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry32)
try {
    $wrong = $registry32.CreateSubKey('SOFTWARE\ODBC\ODBC.INI\RedshiftProd')
    try {
        $wrong.SetValue('Server', 'wrong-view.invalid')
        $wrong.SetValue('Port', '1')
        $wrong.SetValue('Driver', 'C:\missing-32-bit-driver.dll')
    } finally { $wrong.Dispose() }
} finally { $registry32.Dispose() }

# W2 deliberately unusable defaults prove that explicit attributes override DSNs.
$w2Key = 'HKLM:\SOFTWARE\ODBC\ODBC.INI\ODBCPP_W2'
New-Item $w2Key -Force | Out-Null
foreach ($entry in @{ Driver = $driver; Server = '127.0.0.1'; Port = '1'; Database = 'wrong'; UID = 'wrong'; PWD = 'wrong'; SSL = '0' }.GetEnumerator()) {
    New-ItemProperty $w2Key -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
}
New-ItemProperty $dsnsKey -Name ODBCPP_W2 -Value $driverName -PropertyType String -Force | Out-Null
& "$env:PGBIN/psql.exe" -X -v ON_ERROR_STOP=1 -c "CREATE ROLE odbcpp_w2 LOGIN PASSWORD 'w2;secret}tail'"
& "$env:PGBIN/psql.exe" -X -v ON_ERROR_STOP=1 -c "CREATE ROLE odbcpp_w2_unicode LOGIN PASSWORD U&'caf\00e9;secret}tail'"
