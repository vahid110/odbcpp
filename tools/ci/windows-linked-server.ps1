param([string]$Artifacts = 'beta-artifact')
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$logs = Join-Path $env:RUNNER_TEMP 'linked-server-evidence'
New-Item $logs -ItemType Directory -Force | Out-Null
$installLog = Join-Path $logs 'driver-install.log'
$sqlDownload = Join-Path $env:RUNNER_TEMP 'SQL2022-SSEI-Dev.exe'
$sqlMedia = Join-Path $env:RUNNER_TEMP 'sql-server-media'
$driverMsi = @(Get-ChildItem $Artifacts -Recurse -Filter 'odbcpp-postgresql-*-x64.msi')

function Assert([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}

function Invoke-Sql([string]$CommandText) {
    $connection = [System.Data.SqlClient.SqlConnection]::new(
        'Server=localhost;Integrated Security=True;Encrypt=False;TrustServerCertificate=True;Connect Timeout=30')
    try {
        $connection.Open()
        $command = $connection.CreateCommand()
        $command.CommandTimeout = 90
        $command.CommandText = $CommandText
        $table = [System.Data.DataTable]::new()
        $adapter = [System.Data.SqlClient.SqlDataAdapter]::new($command)
        [void]$adapter.Fill($table)
        return ,$table
    } finally {
        if ($connection) { $connection.Dispose() }
    }
}

function Wait-SqlServer {
    $deadline = (Get-Date).AddMinutes(3)
    do {
        try {
            [void](Invoke-Sql 'SELECT 1')
            return
        } catch {
            Start-Sleep -Seconds 2
        }
    } while ((Get-Date) -lt $deadline)
    throw 'SQL Server did not become ready within three minutes'
}

Assert ($driverMsi.Count -eq 1) 'Expected exactly one PostgreSQL beta MSI'
$msi = $driverMsi[0].FullName
$msiProcess = Start-Process msiexec.exe -ArgumentList @(
    '/i', "`"$msi`"", '/qn', '/norestart', '/l*v', "`"$installLog`"
) -Wait -PassThru
Assert ($msiProcess.ExitCode -eq 0) "Driver MSI installation returned $($msiProcess.ExitCode)"

& "$PSScriptRoot/windows-postgres.ps1" -Action Start -UseInstalledDriver
try {
    & "$env:PGBIN/psql.exe" -X -v ON_ERROR_STOP=1 -c @'
CREATE TABLE public.odbcpp_g8_linked_server (
    id integer PRIMARY KEY,
    text_value varchar(64) NOT NULL,
    nullable_text text,
    exact_value numeric(12,3) NOT NULL,
    timestamp_value timestamp NOT NULL
);
INSERT INTO public.odbcpp_g8_linked_server VALUES
    (1, U&'Gr\00FC\00DFe \6771\4EAC', NULL, 123456.789, timestamp '2026-09-29 12:34:56'),
    (2, 'plain', 'present', -0.125, timestamp '2000-01-02 03:04:05');
CREATE ROLE odbcpp_g8 LOGIN PASSWORD 'g8-linked-server-password';
GRANT CONNECT ON DATABASE postgres TO odbcpp_g8;
GRANT USAGE ON SCHEMA public TO odbcpp_g8;
GRANT SELECT ON public.odbcpp_g8_linked_server TO odbcpp_g8;
'@

    $driverKey = 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\ODBCPP PostgreSQL'
    $driver = (Get-ItemProperty $driverKey).Driver
    $dsnName = 'ODBCPP_G8_POSTGRESQL'
    $dsnKey = "HKLM:\SOFTWARE\ODBC\ODBC.INI\$dsnName"
    $dataSources = 'HKLM:\SOFTWARE\ODBC\ODBC.INI\ODBC Data Sources'
    New-Item $dsnKey -Force | Out-Null
    foreach ($entry in @{
        Driver = $driver; Server = '127.0.0.1'; Port = '5432'; Database = 'postgres'
        UID = 'odbcpp_g8'; PWD = 'g8-linked-server-password'; SSL = '0'
    }.GetEnumerator()) {
        New-ItemProperty $dsnKey -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
    }
    New-Item $dataSources -Force | Out-Null
    New-ItemProperty $dataSources -Name $dsnName -Value 'ODBCPP PostgreSQL' -PropertyType String -Force | Out-Null

    $downloadWatch = [Diagnostics.Stopwatch]::StartNew()
    Invoke-WebRequest 'https://aka.ms/sqlserver2022developer' -OutFile $sqlDownload
    $signature = Get-AuthenticodeSignature $sqlDownload
    Assert ($signature.Status -eq 'Valid') 'SQL Server bootstrapper signature is invalid'
    Assert ($signature.SignerCertificate.Subject -match 'Microsoft') 'SQL Server bootstrapper is not signed by Microsoft'
    New-Item $sqlMedia -ItemType Directory -Force | Out-Null
    $download = Start-Process $sqlDownload -ArgumentList @(
        '/Quiet', '/Action=Download', "/MediaPath=$sqlMedia", '/MediaType=ISO'
    ) -Wait -PassThru
    Assert ($download.ExitCode -eq 0) "SQL Server media download returned $($download.ExitCode)"
    $downloadWatch.Stop()

    $iso = @(Get-ChildItem $sqlMedia -Filter '*.iso')
    Assert ($iso.Count -eq 1) 'SQL Server downloader did not produce exactly one ISO'
    $disk = Mount-DiskImage $iso[0].FullName -PassThru
    try {
        $volume = $disk | Get-Volume
        $setup = "$($volume.DriveLetter):\setup.exe"
        Assert (Test-Path $setup) 'SQL Server setup.exe is missing from downloaded media'
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        $setupWatch = [Diagnostics.Stopwatch]::StartNew()
        $setupProcess = Start-Process $setup -ArgumentList @(
            '/Q', '/ACTION=Install', '/FEATURES=SQLEngine', '/INSTANCENAME=MSSQLSERVER',
            "/SQLSYSADMINACCOUNTS=`"$identity`"", '/SQLSVCSTARTUPTYPE=Automatic',
            '/TCPENABLED=0', '/NPENABLED=0', '/UPDATEENABLED=False',
            '/IACCEPTSQLSERVERLICENSETERMS'
        ) -Wait -PassThru
        Assert ($setupProcess.ExitCode -eq 0) "SQL Server setup returned $($setupProcess.ExitCode)"
        $setupWatch.Stop()
    } finally {
        Dismount-DiskImage $iso[0].FullName -ErrorAction SilentlyContinue
    }

    Wait-SqlServer
    $server = Invoke-Sql "SELECT CAST(SERVERPROPERTY('ProductVersion') AS nvarchar(128)) AS product_version, CAST(SERVERPROPERTY('Edition') AS nvarchar(128)) AS edition"
    Assert ($server.Rows.Count -eq 1) 'SQL Server identity query returned no row'

    [void](Invoke-Sql @"
EXEC master.dbo.sp_MSset_oledb_prop N'MSDASQL', N'AllowInProcess', 1;
EXEC master.dbo.sp_addlinkedserver
    @server=N'ODBCPP_POSTGRESQL', @srvproduct=N'ODBCPP PostgreSQL',
    @provider=N'MSDASQL', @datasrc=N'$dsnName';
EXEC master.dbo.sp_droplinkedsrvlogin @rmtsrvname=N'ODBCPP_POSTGRESQL', @locallogin=NULL;
EXEC master.dbo.sp_addlinkedsrvlogin
    @rmtsrvname=N'ODBCPP_POSTGRESQL', @useself=N'False', @locallogin=NULL,
    @rmtuser=N'odbcpp_g8', @rmtpassword=N'g8-linked-server-password';
EXEC master.dbo.sp_serveroption N'ODBCPP_POSTGRESQL', N'data access', N'true';
"@)

    $metadata = Invoke-Sql "EXEC master.dbo.sp_tables_ex @table_server=N'ODBCPP_POSTGRESQL', @table_name=N'odbcpp_g8_linked_server'"
    Assert (@($metadata.Rows | Where-Object { $_.TABLE_NAME -eq 'odbcpp_g8_linked_server' }).Count -ge 1) 'Linked-server metadata did not expose the fixture table'

    $rows = Invoke-Sql @"
SELECT id, text_value, nullable_text, exact_value, timestamp_value
FROM OPENQUERY(ODBCPP_POSTGRESQL,
    'SELECT id, text_value, nullable_text, exact_value, timestamp_value FROM public.odbcpp_g8_linked_server ORDER BY id')
"@
    Assert ($rows.Rows.Count -eq 2) 'OPENQUERY returned the wrong row count'
    Assert ([int]$rows.Rows[0].id -eq 1) 'OPENQUERY integer value mismatch'
    Assert ([string]$rows.Rows[0].text_value -eq 'Grüße 東京') 'OPENQUERY Unicode value mismatch'
    Assert ($rows.Rows[0].IsNull('nullable_text')) 'OPENQUERY NULL value mismatch'
    Assert ([decimal]$rows.Rows[0].exact_value -eq [decimal]123456.789) 'OPENQUERY numeric value mismatch'
    Assert ([datetime]$rows.Rows[0].timestamp_value -eq [datetime]'2026-09-29T12:34:56') 'OPENQUERY timestamp value mismatch'
    Assert ([string]$rows.Rows[1].nullable_text -eq 'present') 'OPENQUERY non-NULL value mismatch'

    $fourPart = Invoke-Sql 'SELECT id, text_value FROM [ODBCPP_POSTGRESQL].[postgres].[public].[odbcpp_g8_linked_server] WHERE id = 2'
    Assert ($fourPart.Rows.Count -eq 1 -and [int]$fourPart.Rows[0].id -eq 2) 'Four-part linked-server query mismatch'

    $invalidFailed = $false
    try {
        [void](Invoke-Sql "SELECT * FROM OPENQUERY(ODBCPP_POSTGRESQL, 'SELECT * FROM public.odbcpp_g8_missing')")
    } catch [System.Data.SqlClient.SqlException] {
        $invalidFailed = $true
        Assert ($_.Exception.Message -match 'ODBC|MSDASQL|odbcpp_g8_missing') 'Invalid-query diagnostic lacks provider context'
    }
    Assert $invalidFailed 'Invalid OPENQUERY unexpectedly succeeded'
    $recovery = Invoke-Sql "SELECT id FROM OPENQUERY(ODBCPP_POSTGRESQL, 'SELECT id FROM public.odbcpp_g8_linked_server WHERE id = 1')"
    Assert ($recovery.Rows.Count -eq 1 -and [int]$recovery.Rows[0].id -eq 1) 'OPENQUERY did not recover after an error'

    [ordered]@{
        runnerImage = $env:ImageVersion
        sqlServerVersion = [string]$server.Rows[0].product_version
        sqlServerEdition = [string]$server.Rows[0].edition
        provider = 'MSDASQL'
        driver = (Get-ItemProperty $driverKey).Driver
        driverVersion = (Get-ItemProperty 'HKLM:\SOFTWARE\ODBCPP\PostgreSQL').Version
        postgresqlVersion = (& "$env:PGBIN/psql.exe" -X -At -c 'SHOW server_version')
        mediaDownloadSeconds = [math]::Round($downloadWatch.Elapsed.TotalSeconds, 1)
        sqlSetupSeconds = [math]::Round($setupWatch.Elapsed.TotalSeconds, 1)
        assertions = @('metadata', 'OPENQUERY typed values', 'four-part name', 'invalid query', 'recovery')
    } | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $logs 'result.json') -Encoding utf8
    Write-Host 'SQL Server MSDASQL linked-server and OPENQUERY acceptance passed.'
} finally {
    $setupRoot = Join-Path ${env:ProgramFiles} 'Microsoft SQL Server\160\Setup Bootstrap\Log'
    if (Test-Path $setupRoot) {
        Copy-Item $setupRoot (Join-Path $logs 'sql-server-setup') -Recurse -Force -ErrorAction SilentlyContinue
    }
    & "$PSScriptRoot/windows-postgres.ps1" -Action Stop
}
