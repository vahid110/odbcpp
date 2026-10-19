# Local native GUI qualification: no registration, installer, live server or elevation.
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build-windows-dsn-pg',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 60)][int]$BuildTimeoutMinutes = 20
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'This runner requires Windows.' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..')).TrimEnd('\', '/')
if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repo $BuildDirectory }
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path.TrimEnd('\', '/')
if (!$build.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Use an owned configured build directory inside this checkout.'
}
$cachePath = Join-Path $build 'CMakeCache.txt'
$cache = @{}
foreach ($line in Get-Content -LiteralPath $cachePath) {
    if ($line -match '^([^#/][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
}
if (!$cache.ContainsKey('CMAKE_HOME_DIRECTORY') -or
    [IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY']).TrimEnd('\', '/') -ine $repo) {
    throw 'The CMake graph must belong to this exact checkout.'
}
if ($cache['TARGET_DATABASE'] -notin @('POSTGRESQL', 'REDSHIFT')) { throw 'Unsupported product graph.' }
function Find-CMakeTool([string]$Name) {
    $command = Get-Command "$Name.exe" -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    $fallback = Join-Path $env:ProgramFiles "CMake/bin/$Name.exe"
    if (Test-Path -LiteralPath $fallback -PathType Leaf) { return $fallback }
    throw "$Name.exe is required; configure the owned product graph first."
}
$cmake = Find-CMakeTool 'cmake'
$ctest = Find-CMakeTool 'ctest'
$helperPath = Join-Path $PSScriptRoot 'windows_local_process.cs'
$helperHash = (Get-FileHash -LiteralPath $helperPath -Algorithm SHA256).Hash
# Key the loaded type by source hash so a second invocation cannot reuse stale code.
$helperNamespace = 'Odbcpp.LocalGui.Process_' + $helperHash.Substring(0, 16)
$helperTypeName = $helperNamespace + '.WindowsLocalProcess'
if ($null -eq ($helperTypeName -as [type])) {
    Add-Type -TypeDefinition ("namespace $helperNamespace {`n" + (Get-Content -LiteralPath $helperPath -Raw) + "`n}")
}
$processRunnerType = $helperTypeName -as [type]
$run = Join-Path $build ('gui-local-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$report = [ordered]@{
    started_utc = [DateTime]::UtcNow.ToString('o')
    checkout = $repo; build = $build; configuration = $Configuration; product = $cache['TARGET_DATABASE']
    cache_sha256 = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash
    runner_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    process_helper_sha256 = $helperHash
    powershell_version = $PSVersionTable.PSVersion.ToString()
    elevated = ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    commands = @(); executables = @(); status = 'RUNNING'
}

# Use CreateProcess (never ShellExecute/RunAs), with hidden helper windows and
# asynchronous output drains. A timeout kills only this held process and its tree.
function Invoke-LocalProcess([string]$File, [string[]]$Arguments, [string]$Label, [int]$TimeoutSeconds) {
    $quoted = foreach ($argument in $Arguments) {
        if ($argument -match '["\r\n]') { throw 'Unsupported quote/newline in a process argument.' }
        '"' + [regex]::Replace($argument, '(\\+)$', '$1$1') + '"'
    }
    $entry = [ordered]@{ file = $File; arguments = $Arguments; label = $Label; timeout_seconds = $TimeoutSeconds }
    $report.commands += $entry
    $execution = $null
    try {
        $execution = $processRunnerType::Run($File, ($quoted -join ' '), $repo, $TimeoutSeconds)
        if (!$execution.CleanupVerified) { throw "Unverified process cleanup for $Label. See $run." }
        if ($execution.TimedOut) { throw "$Label exceeded its finite timeout. See $run." }
        if ($execution.ExitCode -ne 0) { throw "$Label exited $($execution.ExitCode). See $run." }
    }
    catch {
        $entry.error = $_.Exception.ToString()
        $failure = $_.Exception
        while ($null -ne $failure) {
            if ($failure.Data.Contains('Result')) { $execution = $failure.Data['Result']; break }
            $failure = $failure.InnerException
        }
        throw
    }
    finally {
        if ($null -ne $execution) {
            $entry.pid = $execution.ProcessId
            $entry.exit_code = $execution.ExitCode
            $entry.timed_out = $execution.TimedOut
            $entry.cleanup_verified = $execution.CleanupVerified
            $entry.elapsed_milliseconds = $execution.ElapsedMilliseconds
            $execution.Stdout | Set-Content -LiteralPath (Join-Path $run "$Label.stdout.log") -Encoding UTF8
            $execution.Stderr | Set-Content -LiteralPath (Join-Path $run "$Label.stderr.log") -Encoding UTF8
        }
    }
}

# Read RT_MANIFEST as data, without loading code or requiring an SDK tool on PATH.
if ($null -eq ('Odbcpp.LocalGui.ManifestReader' -as [type])) {
    Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace Odbcpp.LocalGui {
    public static class ManifestReader {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        private static extern IntPtr LoadLibraryExW(string name, IntPtr file, uint flags);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern bool FreeLibrary(IntPtr module);
        [DllImport("kernel32.dll", EntryPoint="FindResourceW", SetLastError=true)]
        private static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern uint SizeofResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError=true)] private static extern IntPtr LockResource(IntPtr resource);
        public static string Read(string path) {
            IntPtr module = LoadLibraryExW(path, IntPtr.Zero, 0x22);
            if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                IntPtr resource = FindResource(module, new IntPtr(1), new IntPtr(24));
                if (resource == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
                uint size = SizeofResource(module, resource);
                if (size == 0 || size > 1048576) throw new InvalidOperationException("Invalid manifest size.");
                IntPtr data = LockResource(LoadResource(module, resource));
                if (data == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
                byte[] bytes = new byte[size]; Marshal.Copy(data, bytes, 0, (int)size);
                return Encoding.UTF8.GetString(bytes).TrimStart('\uFEFF');
            } finally { FreeLibrary(module); }
        }
    }
}
'@
}
$testEnvironment = [ordered]@{
    GTEST_FILTER = '*'; GTEST_REPEAT = '1'; GTEST_FAIL_FAST = '0'
    GTEST_TOTAL_SHARDS = $null; GTEST_SHARD_INDEX = $null; GTEST_SHARD_STATUS_FILE = $null
    GTEST_ALSO_RUN_DISABLED_TESTS = '0'; GTEST_BREAK_ON_FAILURE = '0'
    GTEST_THROW_ON_FAILURE = '0'; GTEST_CATCH_EXCEPTIONS = '1'
}
$savedEnvironment = @{}
foreach ($name in @('PATH', 'ODBCPP_GUI_CAPTURE_DIR')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$savedGtestEnvironment = @{}
foreach ($variable in Get-ChildItem Env: | Where-Object Name -Like 'GTEST_*') {
    $savedGtestEnvironment[$variable.Name] = $variable.Value
}
function Set-ProcessEnvironment([string]$Name, $Value) {
    # PowerShell can coerce $null to an empty string; GoogleTest treats an empty
    # shard-status variable as present. Pass an actual CLR null for deletion.
    if ($null -eq $Value) { [Environment]::SetEnvironmentVariable($Name, [NullString]::Value, 'Process') }
    else { [Environment]::SetEnvironmentVariable($Name, [string]$Value, 'Process') }
}
try {
    # Do not inherit filter, sharding, repeat, flag-file or list-only selection.
    foreach ($name in $savedGtestEnvironment.Keys) { Set-ProcessEnvironment $name $null }
    foreach ($name in $testEnvironment.Keys) { Set-ProcessEnvironment $name $testEnvironment[$name] }
    $report.test_environment = $testEnvironment
    Invoke-LocalProcess $cmake @('--build', $build, '--config', $Configuration, '--target', 'odbcpp_setup', 'test_backend_provider', 'test_setup_model', 'test_setup_dialog', '--parallel', '2') 'build' ($BuildTimeoutMinutes * 60)
    $selection = '^(test_backend_provider|test_setup_model|test_setup_dialog)$'
    Invoke-LocalProcess $ctest @('--test-dir', $build, '-C', $Configuration, '-R', $selection, '--show-only=json-v1') 'inventory' 30
    $inventory = Get-Content -LiteralPath (Join-Path $run 'inventory.stdout.log') -Raw | ConvertFrom-Json
    $expected = @('test_backend_provider', 'test_setup_model', 'test_setup_dialog')
    if (@($inventory.tests).Count -ne 3 -or @(Compare-Object $expected @($inventory.tests.name)).Count -ne 0) {
        throw 'The configured graph must contain exactly the three owning test suites.'
    }
    foreach ($test in $inventory.tests) {
        $executable = $test.command[0]
        $manifest = [Odbcpp.LocalGui.ManifestReader]::Read($executable)
        $manifest | Set-Content -LiteralPath (Join-Path $run ($test.name + '.manifest')) -Encoding UTF8
        $settings = [Xml.XmlReaderSettings]::new()
        $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $reader = [Xml.XmlReader]::Create([IO.StringReader]::new($manifest), $settings)
        try { $xml = [Xml.XmlDocument]::new(); $xml.XmlResolver = $null; $xml.Load($reader) }
        finally { $reader.Dispose() }
        $level = $xml.SelectNodes("//*[local-name()='requestedExecutionLevel']")
        if ($level.Count -ne 1 -or $level[0].GetAttribute('level') -cne 'asInvoker' -or $level[0].GetAttribute('uiAccess') -cne 'false') {
            throw "$($test.name) must explicitly run asInvoker with uiAccess=false."
        }
        $report.executables += [ordered]@{ suite = $test.name; path = $executable; sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash; execution_level = 'asInvoker'; ui_access = $false }
    }
    if ($cache.ContainsKey('OPENSSL_ROOT_DIR')) {
        $opensslBin = Join-Path $cache['OPENSSL_ROOT_DIR'] 'bin'
        if (Test-Path -LiteralPath $opensslBin -PathType Container) { $env:PATH = $opensslBin + ';' + $savedEnvironment['PATH'] }
    }
    New-Item -ItemType Directory -Path (Join-Path $run 'gtest'), (Join-Path $run 'captures') | Out-Null
    $env:GTEST_OUTPUT = 'xml:' + (Join-Path $run 'gtest') + [IO.Path]::DirectorySeparatorChar
    $env:ODBCPP_GUI_CAPTURE_DIR = Join-Path $run 'captures'
    Invoke-LocalProcess $ctest @('--test-dir', $build, '-C', $Configuration, '-R', $selection, '--no-tests=error', '--output-on-failure', '--timeout', '30', '-j', '1', '--output-junit', (Join-Path $run 'ctest.xml')) 'tests' 120
    $report.status = 'PASS'
    Write-Host "Local DSN GUI checks passed. Evidence: $run"
}
catch {
    $report.status = 'FAIL'
    $report.error = $_.Exception.Message
    throw
}
finally {
    foreach ($name in $savedEnvironment.Keys) { Set-ProcessEnvironment $name $savedEnvironment[$name] }
    foreach ($variable in Get-ChildItem Env: | Where-Object Name -Like 'GTEST_*') { Set-ProcessEnvironment $variable.Name $null }
    foreach ($name in $savedGtestEnvironment.Keys) { Set-ProcessEnvironment $name $savedGtestEnvironment[$name] }
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding UTF8
}
