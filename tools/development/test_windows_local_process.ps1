# Offline ownership tests for the local hidden-process runner. No registry/admin/network.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$run = Join-Path $repo ('build-gui-evidence/process-runner-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$fixtureDirectory = Join-Path $run 'quoted fixture directory'
New-Item -ItemType Directory -Path $fixtureDirectory | Out-Null
$fixture = Join-Path $fixtureDirectory 'process fixture.exe'
$helperSource = Join-Path $PSScriptRoot 'windows_local_process.cs'
$helperHash = (Get-FileHash -LiteralPath $helperSource -Algorithm SHA256).Hash
$testHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
$helperNamespace = 'Odbcpp.LocalGui.Process_' + $helperHash.Substring(0, 16)
$helperName = $helperNamespace + '.WindowsLocalProcess'
if ($null -eq ($helperName -as [type])) {
    Add-Type -TypeDefinition ('namespace ' + $helperNamespace + " {`n" + (Get-Content -LiteralPath $helperSource -Raw) + "`n}")
}
$helperType = $helperName -as [type]
$fixtureSource = @'
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public static class OwnedFixture {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct Startup {
        public int Size; public string Reserved, Desktop, Title;
        public uint X, Y, XSize, YSize, XChars, YChars, Fill, Flags;
        public ushort Show, ReservedBytes; public IntPtr ReservedData, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)] struct Child { public IntPtr Process, Thread; public uint Id, ThreadId; }
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr GetStdHandle(int kind);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string file, StringBuilder command, IntPtr processSecurity, IntPtr threadSecurity,
        bool inherit, uint flags, IntPtr environment, string directory, ref Startup startup, out Child child);
    public static int Main(string[] args) {
        Console.OutputEncoding = new UTF8Encoding(false);
        if (args[0] == "success") {
            Console.WriteLine("stdout:" + args[1]);
            Console.Error.WriteLine("stderr:" + args[2]);
            return 0;
        }
        if (args[0] == "nonzero") {
            Console.WriteLine("retained stdout"); Console.Error.WriteLine("retained stderr"); return 23;
        }
        if (args[0] == "flood") {
            string payload = new string('x', 100);
            for (int i = 0; i < 10000; ++i) { Console.WriteLine(payload); Console.Error.WriteLine(payload); }
            Console.WriteLine("stdout complete"); Console.Error.WriteLine("stderr complete"); return 0;
        }
        if (args[0] == "sleep") {
            Console.WriteLine("before timeout"); Console.Error.WriteLine("timeout diagnostic"); Thread.Sleep(30000); return 0;
        }
        if (args[0] == "child") {
            Console.WriteLine("inherited child output"); Console.Error.WriteLine("inherited child error");
            File.WriteAllText(args[1], "ready"); Thread.Sleep(30000); return 0;
        }
        if (args[0] == "linger") {
            // Explicitly inherit the root's redirected handles. Process.Start
            // without redirection does not reliably inherit them on .NET Framework.
            Startup startup = new Startup(); startup.Size = Marshal.SizeOf(typeof(Startup)); startup.Flags = 0x100;
            startup.Input = GetStdHandle(-10); startup.Output = GetStdHandle(-11); startup.Error = GetStdHandle(-12);
            SetHandleInformation(startup.Input, 1, 1); SetHandleInformation(startup.Output, 1, 1); SetHandleInformation(startup.Error, 1, 1);
            string file = Process.GetCurrentProcess().MainModule.FileName;
            Child child;
            if (!CreateProcessW(file, new StringBuilder("\"" + file + "\" child \"" + args[1] + "\""),
                IntPtr.Zero, IntPtr.Zero, true, 0x08000000, IntPtr.Zero, null, ref startup, out child)) return 25;
            CloseHandle(child.Thread);
            Stopwatch deadline = Stopwatch.StartNew();
            while (!File.Exists(args[1]) && deadline.ElapsedMilliseconds < 5000) Thread.Sleep(5);
            if (!File.Exists(args[1])) return 24;
            Console.WriteLine("root exits with live child:" + child.Id);
            CloseHandle(child.Process);
            return 0;
        }
        return 99;
    }
}
'@
$fixtureSource | Set-Content -LiteralPath (Join-Path $run 'fixture.cs') -Encoding UTF8
Add-Type -TypeDefinition $fixtureSource -OutputAssembly $fixture -OutputType ConsoleApplication
$results = New-Object Collections.Generic.List[object]
function Check([bool]$Condition, [string]$Description) {
    if (!$Condition) { throw $Description }
}
function Run-Case([string]$Label, [string]$Arguments, [int]$TimeoutSeconds = 10) {
    $result = $helperType::Run($fixture, $Arguments, $run, $TimeoutSeconds)
    $result.Stdout | Set-Content -LiteralPath (Join-Path $run ($Label + '.stdout.log')) -Encoding UTF8
    $result.Stderr | Set-Content -LiteralPath (Join-Path $run ($Label + '.stderr.log')) -Encoding UTF8
    $results.Add([ordered]@{ label = $Label; exit_code = $result.ExitCode; timed_out = $result.TimedOut; pid = $result.ProcessId; elapsed_ms = $result.ElapsedMilliseconds; cleanup_verified = $result.CleanupVerified })
    Check $result.CleanupVerified "$Label must verify zero active owned processes"
    return $result
}
$status = 'FAIL'
try {
    $result = Run-Case 'quoted-success' 'success "spaced argument" "embedded\"quote"'
    Check ($result.ExitCode -eq 0 -and !$result.TimedOut) 'Successful process must return zero'
    Check ($result.Stdout.Contains('stdout:spaced argument')) 'Spaced argument was changed'
    Check ($result.Stderr.Contains('stderr:embedded"quote')) 'Quoted argument was changed'
    $result = Run-Case 'nonzero' 'nonzero'
    Check ($result.ExitCode -eq 23 -and !$result.TimedOut) 'Nonzero exit must be preserved'
    Check ($result.Stdout.Contains('retained stdout') -and $result.Stderr.Contains('retained stderr')) 'Nonzero output must be preserved'
    $result = Run-Case 'concurrent-drains' 'flood'
    Check ($result.ExitCode -eq 0 -and !$result.TimedOut) 'Concurrent drains must avoid pipe deadlock'
    Check ($result.Stdout.Contains('stdout complete') -and $result.Stderr.Contains('stderr complete')) 'Full concurrent output must be retained'
    $result = Run-Case 'timeout' 'sleep' 1
    Check ($result.TimedOut -and $result.ExitCode -ne 0 -and $result.ElapsedMilliseconds -lt 10000) 'Timeout must terminate and reap within a finite cleanup bound'
    Check ($result.Stdout.Contains('before timeout') -and $result.Stderr.Contains('timeout diagnostic')) 'Timeout output must be retained'
    $ready = Join-Path $fixtureDirectory 'child ready.txt'
    $result = Run-Case 'root-exit-lingering-child' ('linger "' + $ready + '"')
    Check ($result.ExitCode -eq 0 -and !$result.TimedOut -and $result.ElapsedMilliseconds -lt 10000) 'Root exit must reap residual child without waiting for inherited pipe EOF'
    Check ($result.Stdout.Contains('inherited child output') -and $result.Stderr.Contains('inherited child error')) 'Inherited child output must be retained'
    $failed = $false
    try { $helperType::Run((Join-Path $run 'absent.exe'), '', $run, 1) | Out-Null }
    catch {
        $errorObject = $_.Exception
        while ($null -ne $errorObject.InnerException -and !$errorObject.Data.Contains('Result')) { $errorObject = $errorObject.InnerException }
        Check ($errorObject.Data.Contains('Result') -and $errorObject.Data['Result'].CleanupVerified) 'Launch exception must carry verified cleanup evidence'
        $failed = $true
    }
    Check $failed 'Missing executable must throw'
    $status = 'PASS'
    Write-Host "Windows local process tests passed. Evidence: $run"
}
finally {
    [ordered]@{ status = $status; powershell = $PSVersionTable.PSVersion.ToString(); helper_sha256 = $helperHash; test_source_sha256 = $testHash; cases = @($results.ToArray()); finished_utc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'results.json') -Encoding UTF8
}
