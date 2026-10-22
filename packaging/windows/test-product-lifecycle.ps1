# Native x64 MSI coexistence/upgrade/removal and owned User DSNs. No database/cloud/GUI calls.
param(
    [Parameter(Mandatory=$true)][string]$PostgreSqlPackage,
    [Parameter(Mandatory=$true)][string]$RedshiftPackage,
    [Parameter(Mandatory=$true)][string]$RedshiftUpgrade,
    [Parameter(Mandatory=$true)][string]$RedshiftRollbackFixture,
    [Parameter(Mandatory=$true)][string]$LoaderExecutable,
    [Parameter(Mandatory=$true)][string]$PostgreSqlSetupBuild,
    [Parameter(Mandatory=$true)][string]$RedshiftSetupBuild,
    [string]$CTestExecutable = 'C:/Program Files/CMake/bin/ctest.exe',
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
$report = [ordered]@{ started_utc = [DateTime]::UtcNow.ToString('o'); operations = @(); setup_tests = @(); cleanup = @(); result = 'RUNNING'; source_files = @(); cloud_operations = 0; database_operations = 0 }
$savedGtest = @{}
foreach ($entry in Get-ChildItem Env: | Where-Object Name -Like 'GTEST_*') { $savedGtest[$entry.Name] = $entry.Value }
$products = @{
    PostgreSQL = @{ upgrade = '{BC23230D-B189-4CF8-81D2-11ED5F55FC6F}'; directory = Join-Path $env:ProgramFiles 'ODBCPP/PostgreSQL'; files = @() }
    Redshift = @{ upgrade = '{7CF4F823-59A8-481B-B940-CD9AEF80A3EA}'; directory = Join-Path $env:ProgramFiles 'ODBCPP/Redshift'; files = @() }
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
function Package-Files($Database) {
    $view = $Database.OpenView('SELECT `FileName` FROM `File`')
    try {
        [void]$view.Execute()
        while ($null -ne ($record = $view.Fetch())) {
            try {
                $name = ($record.StringData(1) -split '\|')[-1]
                Assert ($name -and $name -notmatch '[/\\]' -and $name -notin @('.','..')) 'Unexpected MSI payload filename'
                $name
            } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) }
        }
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
        $products[$Name].files = @(@($products[$Name].files) + @(Package-Files $database) | Sort-Object -Unique)
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
    foreach ($file in $products[$Name].files) {
        Assert (!(Test-Path -LiteralPath (Join-Path $products[$Name].directory $file))) 'Owned installed payload remains'
    }
    foreach ($path in @("HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBCPP $Name", "HKLM:/SOFTWARE/ODBCPP/$Name")) {
        $entry = Get-ItemProperty -LiteralPath $path -ErrorAction SilentlyContinue
        if ($entry) { Assert (@($entry.PSObject.Properties | Where-Object Name -in @('Driver','Setup','OwnedDriver','OwnedSetup','Version')).Count -eq 0) 'Owned registration remains' }
    }
    $mapping = Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBC Drivers'
    Assert ($null -eq $mapping.PSObject.Properties["ODBCPP $Name"]) 'Installed driver mapping remains'
}
function Setup-Graph([string]$Build, [string]$Name) {
    $build = (Resolve-Path -LiteralPath $Build).Path
    $cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
    Assert ($cache -match ('(?m)^TARGET_DATABASE:STRING='+$Name.ToUpperInvariant()+'\r?$')) 'Installed-setup graph product differs'
    Assert ($cache -match '(?m)^ODBCPP_INSTALLED_SETUP_TESTS:BOOL=ON\r?$') 'Installed-setup CTest registration must be explicit'
    $arguments = '--test-dir "'+$build+'" -C Release -R "^it_setup_registration$" --show-only=json-v1'
    $execution = $loaderRunner::Run($CTestExecutable, $arguments, $evidence, 30)
    Assert ($execution.CleanupVerified -and !$execution.TimedOut -and $execution.ExitCode -eq 0) 'Setup inventory process did not finish'
    $inventory = $execution.Stdout | ConvertFrom-Json
    Assert (@($inventory.tests).Count -eq 1 -and $inventory.tests[0].name -ceq 'it_setup_registration') 'Unexpected setup inventory'
    $image = $inventory.tests[0].command[0]
    $report.source_files += @{ path=$image; sha256=(Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash }
    $execution.Stdout | Set-Content -LiteralPath (Join-Path $evidence ("setup-$Name-inventory.json")) -Encoding UTF8
    return $build
}
function Run-Setup([string]$Build, [string]$Name) {
    $directory = Join-Path $evidence ("setup-$Name")
    New-Item -ItemType Directory -Path $directory | Out-Null
    $env:GTEST_OUTPUT = 'xml:'+(Join-Path $directory 'gtest.xml')
    $arguments = '--test-dir "'+$Build+'" -C Release -R "^it_setup_registration$" -j1 --no-tests=error --timeout 30 --output-on-failure --output-junit "'+(Join-Path $directory 'ctest.xml')+'" --output-log "'+(Join-Path $directory 'ctest.log')+'"'
    $execution = $loaderRunner::Run($CTestExecutable, $arguments, $evidence, 45)
    ($execution.Stdout+$execution.Stderr) | Set-Content -LiteralPath (Join-Path $directory 'command.log') -Encoding UTF8
    Assert ($execution.CleanupVerified -and !$execution.TimedOut) 'Setup CTest reader cleanup is unknown'
    $passed = $false
    $xmlError = $null
    $xml = Join-Path $directory 'gtest.xml'
    if (Test-Path -LiteralPath $xml) {
        try {
            [xml]$result = Get-Content -LiteralPath $xml -Raw -Encoding UTF8
            $cases = $result.SelectNodes('//testcase')
            $bad = $result.SelectNodes('//testcase[failure or error or skipped or @status="notrun" or @result="suppressed"]')
            $passed = $execution.ExitCode -eq 0 -and $cases.Count -eq 3 -and $bad.Count -eq 0 -and [int]$result.testsuites.GetAttribute('tests') -eq 3
        } catch { $xmlError = $_.Exception.Message }
    }
    $report.setup_tests += @{ product=$Name; exit=$execution.ExitCode; all_three_passed=$passed; xml_error=$xmlError; evidence=$directory }
}
try {
    foreach ($name in $products.Keys) {
        $parent = [IO.DirectoryInfo]::new($products[$name].directory)
        while ($null -ne $parent) {
            if ($parent.Exists) { Assert (($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Refuse reparse install ancestry' }
            $parent = $parent.Parent
        }
        Assert (!(Test-Path -LiteralPath $products[$name].directory)) 'Refuse an existing product directory'
        Assert (!(Test-Path -LiteralPath ("HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBCPP $name"))) 'Refuse an existing driver registration'
        Assert (!(Test-Path -LiteralPath ("HKLM:/SOFTWARE/ODBCPP/$name"))) 'Refuse an existing package owner'
        $mapping = Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/ODBC/ODBCINST.INI/ODBC Drivers'
        Assert ($null -eq $mapping.PSObject.Properties["ODBCPP $name"]) 'Refuse an existing driver mapping'
        $related = $installer.RelatedProducts($products[$name].upgrade)
        # Windows Installer's StringList is enumerable; its COM Count member is
        # not exposed by PowerShell. Enumerate it before checking for collisions.
        try { Assert (@($related).Count -eq 0) 'Refuse an existing product in this upgrade family' }
        finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($related) }
    }
    $pg = Pin-Package $PostgreSqlPackage 'PostgreSQL' '1.0.0'
    $rs = Pin-Package $RedshiftPackage 'Redshift' '1.0.0'
    $upgrade = Pin-Package $RedshiftUpgrade 'Redshift' '1.0.1'
    $rollback = Pin-Package $RedshiftRollbackFixture 'Redshift' '1.0.2'
    $LoaderExecutable = (Resolve-Path -LiteralPath $LoaderExecutable).Path
    $held += [IO.File]::Open($LoaderExecutable, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $report.source_files += @{path = $LoaderExecutable; sha256 = (Get-FileHash -LiteralPath $LoaderExecutable -Algorithm SHA256).Hash}
    $CTestExecutable = (Resolve-Path -LiteralPath $CTestExecutable).Path
    $PostgreSqlSetupBuild = Setup-Graph $PostgreSqlSetupBuild 'PostgreSQL'
    $RedshiftSetupBuild = Setup-Graph $RedshiftSetupBuild 'Redshift'
    foreach ($name in $savedGtest.Keys) { [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process') }
    # The native package loader is /MT and verifies dependencies beside each installed driver.
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $mutated = $true
    Msi $pg '/i' 'install-pg'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $rs '/i' 'install-redshift'; Verify-Installed 'Redshift' '1.0.0'; Verify-Installed 'PostgreSQL' '1.0.0'
    # Collect both independent full owning GoogleTest groups before assessing failures.
    Run-Setup $PostgreSqlSetupBuild 'PostgreSQL'
    Run-Setup $RedshiftSetupBuild 'Redshift'
    Msi $rollback '/i' 'redshift-rollback' 1603
    Assert (Select-String -LiteralPath (Join-Path $evidence 'redshift-rollback.log') -SimpleMatch 'Intentional rollback acceptance failure.' -Quiet) 'Rollback failed before the injection point'
    Verify-Installed 'Redshift' '1.0.0'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $upgrade '/i' 'upgrade-redshift'; Verify-Installed 'Redshift' '1.0.1'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $rs '/i' 'redshift-downgrade' 1603
    Verify-Installed 'Redshift' '1.0.1'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $upgrade '/x' 'remove-redshift'; Verify-Absent 'Redshift'; Verify-Installed 'PostgreSQL' '1.0.0'
    Msi $pg '/x' 'remove-pg'; Verify-Absent 'PostgreSQL'
    Assert (@($report.setup_tests | Where-Object { !$_.all_three_passed }).Count -eq 0) 'Installed setup tests failed; both products were collected'
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
            try { Verify-Absent $name; $report.cleanup += @{product=$name; owned_installation_absent=$true; payload_files_checked=$products[$name].files.Count} }
            catch { $report.cleanup += @{product=$name; error=$_.ToString()} }
        }
    }
    $env:PATH = $savedPath
    foreach ($entry in Get-ChildItem Env: | Where-Object Name -Like 'GTEST_*') { [Environment]::SetEnvironmentVariable($entry.Name, [NullString]::Value, 'Process') }
    foreach ($name in $savedGtest.Keys) { [Environment]::SetEnvironmentVariable($name, $savedGtest[$name], 'Process') }
    foreach ($stream in $held) { $stream.Dispose() }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
    if ($null -ne $active -and !$active.HasExited) { $report.cleanup += @{error='MSI operation still active';pid=$active.Id} }
    if (@($report.cleanup | Where-Object { $_.ContainsKey('error') }).Count) { $report.result = 'FAIL' }
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $evidence 'summary.json') -Encoding UTF8
}
Assert ($report.result -ceq 'PASS') 'Native lifecycle or owned cleanup did not pass'
