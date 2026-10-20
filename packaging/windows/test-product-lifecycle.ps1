# Local x64 MSI coexistence/upgrade/removal. No database, cloud, DSN or GUI actions.
param(
    [Parameter(Mandatory=$true)][string]$PostgreSqlPackage,
    [Parameter(Mandatory=$true)][string]$RedshiftPackage,
    [Parameter(Mandatory=$true)][string]$RedshiftUpgrade,
    [Parameter(Mandatory=$true)][string]$RedshiftRollbackFixture,
    [Parameter(Mandatory=$true)][string]$LoaderExecutable,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
Assert ([Environment]::Is64BitProcess -and $env:OS -eq 'Windows_NT') 'Use native x64 Windows'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
Assert (([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) 'Approved local elevated token required'
Assert (!(Test-Path -LiteralPath $EvidenceDirectory)) 'Preserve existing lifecycle evidence'
New-Item -ItemType Directory -Path $EvidenceDirectory | Out-Null
$evidence = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$installer = New-Object -ComObject WindowsInstaller.Installer
$held = @(); $active = $null; $mutated = $false; $savedPath = $env:PATH
$report = [ordered]@{ started_utc = [DateTime]::UtcNow.ToString('o'); operations = @(); cleanup = @(); result = 'RUNNING'; source_files = @(); cloud_operations = 0; database_operations = 0 }
$products = @{
    PostgreSQL = @{ upgrade = '{BC23230D-B189-4CF8-81D2-11ED5F55FC6F}'; directory = Join-Path $env:ProgramFiles 'ODBCPP/PostgreSQL' }
    Redshift = @{ upgrade = '{7CF4F823-59A8-481B-B940-CD9AEF80A3EA}'; directory = Join-Path $env:ProgramFiles 'ODBCPP/Redshift' }
}
$processHelper = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../tools/development/windows_local_process.cs')).Path
$helperHash = (Get-FileHash -LiteralPath $processHelper -Algorithm SHA256).Hash
$helperNamespace = 'Odbcpp.Package.Lifecycle_' + $helperHash.Substring(0,16)
$helperType = $helperNamespace + '.WindowsLocalProcess'
if ($null -eq ($helperType -as [type])) {
    Add-Type -TypeDefinition ("namespace $helperNamespace {`n"+(Get-Content -LiteralPath $processHelper -Raw)+"`n}")
}
$loaderRunner = $helperType -as [type]
$report.source_files += @{ path=$processHelper; sha256=$helperHash }
function Property($Database, [string]$Name) {
    $view = $Database.OpenView('SELECT `Value` FROM `Property` WHERE `Property` = ''' + $Name + '''')
    try {
        [void]$view.Execute(); $record = $view.Fetch()
        try { Assert ($null -ne $record) "Missing MSI property $Name"; return $record.StringData(1) }
        finally { if ($record) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) } }
    } finally { [void]$view.Close(); [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) }
}
function Pin-Package([string]$Path, [string]$Name, [string]$Version) {
    $path = (Resolve-Path -LiteralPath $Path).Path
    $item = Get-Item -LiteralPath $path
    Assert (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Refuse a reparse MSI'
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $script:held += $stream
    $database = $installer.OpenDatabase($path, 0)
    try {
        Assert ((Property $database 'ProductName') -ceq "ODBCPP $Name Beta (x64)") 'Unexpected MSI product'
        Assert ((Property $database 'ProductVersion') -ceq $Version) 'Unexpected MSI version'
        Assert ((Property $database 'UpgradeCode') -ieq $products[$Name].upgrade) 'Unexpected upgrade family'
        $code = Property $database 'ProductCode'
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) }
    Assert ($installer.ProductState($code) -eq -1) 'Package already registered; refuse existing installation'
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $report.source_files += @{ path = $path; sha256 = $hash; product = $Name; version = $Version; product_code = $code }
    return @{ path = $path; product = $Name; version = $Version; code = $code }
}
function Msi($Package, [string]$Action, [string]$Label, [int]$Expected = 0) {
    $log = Join-Path $evidence ($Label+'.log')
    $script:active = Start-Process -FilePath "$env:SystemRoot/System32/msiexec.exe" -WindowStyle Hidden -ArgumentList @($Action, ('"'+$Package.path+'"'), '/qn', '/norestart', '/l*v', ('"'+$log+'"')) -PassThru
    if (!$active.WaitForExit(180000)) { throw 'MSI operation still active; preserve evidence and do not mutate another installation' }
    $active.Refresh(); $exitCode = $active.ExitCode
    $report.operations += @{ label = $Label; action = $Action; product = $Package.product; version = $Package.version; exit = $exitCode; expected = $Expected; log = $log }
    $active.Dispose(); $script:active = $null
    Assert ($exitCode -eq $Expected) "$Label returned $exitCode, expected $Expected"
}
function Verify-Installed([string]$Name, [string]$Version) {
    $directory = $products[$Name].directory
    $key = Get-ItemProperty -LiteralPath ("HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBCPP $Name")
    $owner = Get-ItemProperty -LiteralPath ("HKLM:/SOFTWARE/ODBCPP/$Name")
    Assert ($key.Driver -ieq (Join-Path $directory 'odbcpp.dll') -and $key.Setup -ieq (Join-Path $directory 'odbcpp_setup.dll')) 'Installed registry paths differ'
    Assert ($owner.Version -ceq $Version -and $owner.OwnedDriver -ieq $key.Driver -and $owner.OwnedSetup -ieq $key.Setup) 'Installed ownership/version differs'
    $inventory = Get-Content -LiteralPath (Join-Path $directory 'inventory.json') -Raw | ConvertFrom-Json
    Assert ($inventory.driverName -ceq "ODBCPP $Name" -and $inventory.targetDatabase -ceq $Name.ToUpperInvariant()) 'Installed inventory product differs'
    foreach ($file in $inventory.files) {
        Assert ((Get-FileHash -LiteralPath (Join-Path $directory $file.file) -Algorithm SHA256).Hash -ceq $file.sha256) 'Installed payload hash differs'
    }
    $ssl = @($inventory.cryptoDependencies | Where-Object role -eq 'ssl')[0].file
    $crypto = @($inventory.cryptoDependencies | Where-Object role -eq 'crypto')[0].file
    $loaderLog = Join-Path $evidence ("loader-$Name-$Version-"+[guid]::NewGuid().ToString('N')+'.log')
    $arguments = @($directory,$ssl,$crypto) | ForEach-Object { Assert ($_ -notmatch '["\r\n]') 'Unsupported loader argument'; '"'+$_+'"' }
    $execution = $loaderRunner::Run($LoaderExecutable, ($arguments -join ' '), $evidence, 30)
    ($execution.Stdout+$execution.Stderr) | Set-Content -LiteralPath $loaderLog -Encoding UTF8
    Assert ($execution.CleanupVerified -and !$execution.TimedOut -and $execution.ExitCode -eq 0) 'Installed driver/setup or app-local runtimes did not load cleanly'
}
function Verify-Absent([string]$Name) {
    Assert (!(Test-Path -LiteralPath (Join-Path $products[$Name].directory 'odbcpp.dll'))) 'Owned installed driver remains'
    foreach ($path in @("HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBCPP $Name", "HKLM:/SOFTWARE/ODBCPP/$Name")) {
        $entry = Get-ItemProperty -LiteralPath $path -ErrorAction SilentlyContinue
        if ($entry) { Assert (@($entry.PSObject.Properties | Where-Object Name -in @('Driver','Setup','OwnedDriver','OwnedSetup','Version')).Count -eq 0) 'Owned registration remains' }
    }
    $mapping = Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBC Drivers'
    Assert ($null -eq $mapping.PSObject.Properties["ODBCPP $Name"]) 'Installed driver mapping remains'
}
try {
    foreach ($name in $products.Keys) {
        Assert (!(Test-Path -LiteralPath $products[$name].directory)) 'Refuse an existing product directory'
        Assert (!(Test-Path -LiteralPath ("HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBCPP $name"))) 'Refuse an existing driver registration'
        Assert (!(Test-Path -LiteralPath ("HKLM:/SOFTWARE/ODBCPP/$name"))) 'Refuse an existing package owner'
        $mapping = Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBC Drivers'
        Assert ($null -eq $mapping.PSObject.Properties["ODBCPP $name"]) 'Refuse an existing driver mapping'
        $related = $installer.RelatedProducts($products[$name].upgrade)
        try { Assert ($related.Count -eq 0) 'Refuse an existing product in this upgrade family' }
        finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($related) }
    }
    $pg = Pin-Package $PostgreSqlPackage 'PostgreSQL' '1.0.0'
    $rs = Pin-Package $RedshiftPackage 'Redshift' '1.0.0'
    $upgrade = Pin-Package $RedshiftUpgrade 'Redshift' '1.0.1'
    $rollback = Pin-Package $RedshiftRollbackFixture 'Redshift' '1.0.2'
    $LoaderExecutable = (Resolve-Path -LiteralPath $LoaderExecutable).Path
    $report.source_files += @{path = $LoaderExecutable; sha256 = (Get-FileHash -LiteralPath $LoaderExecutable -Algorithm SHA256).Hash}
    # The native package loader is /MT and verifies dependencies beside each installed driver.
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $mutated = $true
    Msi $pg '/i' 'install-pg'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $rs '/i' 'install-redshift'; Verify-Installed 'Redshift' '1.0.0'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $rollback '/i' 'redshift-rollback' 1603
    Assert (Select-String -LiteralPath (Join-Path $evidence 'redshift-rollback.log') -SimpleMatch 'Intentional rollback acceptance failure.' -Quiet) 'Rollback failed before the injection point'
    Verify-Installed 'Redshift' '1.0.0'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $upgrade '/i' 'upgrade-redshift'; Verify-Installed 'Redshift' '1.0.1'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $rs '/i' 'redshift-downgrade' 1603
    Verify-Installed 'Redshift' '1.0.1'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $upgrade '/x' 'remove-redshift'; Verify-Absent 'Redshift'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $pg '/x' 'remove-pg'; Verify-Absent 'PostgreSQL'
    $report.result = 'PASS'
} catch {
    $report.result = 'FAIL'; $report.error = $_.ToString(); $report.stack = $_.ScriptStackTrace
    throw
} finally {
    if ($mutated -and ($null -eq $active -or $active.HasExited)) {
        # Only fixed, absence-checked product codes from these held input packages.
        foreach ($package in @($rollback,$upgrade,$rs,$pg)) {
            if ($null -ne $package -and $installer.ProductState($package.code) -eq 5) {
                try { Msi $package '/x' ('cleanup-'+$package.product+'-'+$package.version) }
                catch { $report.cleanup += @{ product=$package.product; error=$_.ToString() } }
            }
        }
        foreach ($name in $products.Keys) {
            try { Verify-Absent $name; $report.cleanup += @{product=$name; owned_installation_absent=$true} }
            catch { $report.cleanup += @{product=$name; error=$_.ToString()} }
        }
    }
    $env:PATH = $savedPath
    foreach ($stream in $held) { $stream.Dispose() }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
    if ($null -ne $active -and !$active.HasExited) { $report.cleanup += @{error='MSI operation still active';pid=$active.Id} }
    if (@($report.cleanup | Where-Object { $_.ContainsKey('error') }).Count) { $report.result = 'FAIL' }
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $evidence 'summary.json') -Encoding UTF8
}
Assert ($report.result -ceq 'PASS') 'Native lifecycle or owned cleanup did not pass'
