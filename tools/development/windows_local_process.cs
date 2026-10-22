using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

[Serializable]
public sealed class WindowsLocalProcessResult {
    public string Stdout = "";
    public string Stderr = "";
    public int ExitCode = -1;
    public bool TimedOut;
    public uint ProcessId;
    public long ElapsedMilliseconds;
    public bool CleanupVerified;
}

// Local, non-elevating, finite execution. Each invocation owns exactly one job.
public static class WindowsLocalProcess {
    const uint Suspended = 4, NoWindow = 0x08000000, ExtendedStartup = 0x00080000;
    const uint KillOnJobClose = 0x2000, StdHandles = 0x100, InfiniteFailure = 0xffffffff;
    const uint WaitObject = 0, WaitTimeout = 258, StillActive = 259;
    static readonly IntPtr Invalid = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential)] struct SecurityAttributes {
        public int Length; public IntPtr Descriptor;
        [MarshalAs(UnmanagedType.Bool)] public bool Inherit;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct StartupInfo {
        public int Size; public string Reserved, Desktop, Title;
        public uint X, Y, XSize, YSize, XChars, YChars, Fill, Flags;
        public ushort ShowWindow, ReservedBytes; public IntPtr ReservedData, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)] struct StartupInfoEx {
        public StartupInfo Startup; public IntPtr Attributes;
    }
    [StructLayout(LayoutKind.Sequential)] struct ProcessInformation {
        public IntPtr Process, Thread; public uint ProcessId, ThreadId;
    }
    [StructLayout(LayoutKind.Sequential)] struct BasicLimits {
        public long ProcessTime, JobTime; public uint Flags;
        public UIntPtr MinimumWorkingSet, MaximumWorkingSet; public uint ActiveLimit;
        public UIntPtr Affinity; public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct IoCounters {
        public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] struct ExtendedLimits {
        public BasicLimits Basic; public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [StructLayout(LayoutKind.Sequential)] struct Accounting {
        public long UserTime, KernelTime, PeriodUserTime, PeriodKernelTime;
        public uint Faults, TotalProcesses, ActiveProcesses, TerminatedProcesses;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, int info, ref ExtendedLimits limits, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int info, out Accounting accounting, uint size, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CreatePipe(out IntPtr read, out IntPtr write, ref SecurityAttributes security, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint sharing, ref SecurityAttributes security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, uint flags, ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
    [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string application, StringBuilder command, IntPtr processSecurity, IntPtr threadSecurity,
        bool inherit, uint flags, IntPtr environment, string directory, ref StartupInfoEx startup, out ProcessInformation process);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    static void Require(bool ok, string operation) {
        if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error(), operation);
    }
    static void Close(ref IntPtr handle) {
        if (handle != IntPtr.Zero && handle != Invalid) CloseHandle(handle);
        handle = IntPtr.Zero;
    }
    static string Quote(string argument) {
        StringBuilder value = new StringBuilder("\"");
        int slashes = 0;
        foreach (char ch in argument) {
            if (ch == '\\') { ++slashes; continue; }
            value.Append('\\', ch == '"' ? slashes * 2 + 1 : slashes);
            value.Append(ch); slashes = 0;
        }
        return value.Append('\\', slashes * 2).Append('"').ToString();
    }
    sealed class Drain : IDisposable {
        readonly StringBuilder captured = new StringBuilder();
        readonly StreamReader reader;
        public readonly Task Completion;
        public Drain(IntPtr pipe) {
            reader = new StreamReader(new FileStream(new SafeFileHandle(pipe, true), FileAccess.Read, 4096, false), Encoding.UTF8, true, 4096);
            Completion = Task.Factory.StartNew(delegate {
                char[] buffer = new char[4096];
                int count;
                while ((count = reader.Read(buffer, 0, buffer.Length)) > 0) {
                    lock (captured) captured.Append(buffer, 0, count);
                }
            }, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }
        public string Snapshot() { lock (captured) return captured.ToString(); }
        public void Dispose() { reader.Dispose(); }
    }
    static void VerifyEmpty(IntPtr job) {
        Stopwatch deadline = Stopwatch.StartNew();
        do {
            Accounting state;
            Require(QueryInformationJobObject(job, 1, out state, (uint)Marshal.SizeOf(typeof(Accounting)), IntPtr.Zero), "Query owned job accounting");
            if (state.ActiveProcesses == 0) return;
            Thread.Sleep(10);
        } while (deadline.ElapsedMilliseconds < 10000);
        throw new TimeoutException("Owned job did not reach zero active processes after termination.");
    }

    public static WindowsLocalProcessResult Run(string file, string arguments, string workingdir, int timeoutseconds) {
        if (String.IsNullOrEmpty(file) || file.IndexOf('\0') >= 0) throw new ArgumentException("An executable path is required.", "file");
        if (arguments != null && arguments.IndexOf('\0') >= 0) throw new ArgumentException("Arguments contain NUL.", "arguments");
        if (timeoutseconds < 1 || timeoutseconds > 86400) throw new ArgumentOutOfRangeException("timeoutseconds");
        WindowsLocalProcessResult result = new WindowsLocalProcessResult();
        Stopwatch elapsed = Stopwatch.StartNew();
        IntPtr job = IntPtr.Zero, stdoutRead = IntPtr.Zero, stdoutWrite = IntPtr.Zero;
        IntPtr stderrRead = IntPtr.Zero, stderrWrite = IntPtr.Zero, input = IntPtr.Zero;
        IntPtr attributes = IntPtr.Zero, inherited = IntPtr.Zero;
        bool attributesInitialized = false, assigned = false;
        ProcessInformation process = new ProcessInformation();
        Drain stdout = null, stderr = null;
        Exception failure = null;
        try {
            job = CreateJobObjectW(IntPtr.Zero, null);
            Require(job != IntPtr.Zero, "Create private process job");
            ExtendedLimits limits = new ExtendedLimits(); limits.Basic.Flags = KillOnJobClose;
            Require(SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits))), "Set kill-on-close job limit");
            SecurityAttributes security = new SecurityAttributes();
            security.Length = Marshal.SizeOf(typeof(SecurityAttributes)); security.Inherit = true;
            Require(CreatePipe(out stdoutRead, out stdoutWrite, ref security, 0), "Create stdout pipe");
            Require(SetHandleInformation(stdoutRead, 1, 0), "Prevent stdout reader inheritance");
            Require(CreatePipe(out stderrRead, out stderrWrite, ref security, 0), "Create stderr pipe");
            Require(SetHandleInformation(stderrRead, 1, 0), "Prevent stderr reader inheritance");
            input = CreateFileW("NUL", 0x80000000, 3, ref security, 3, 0, IntPtr.Zero);
            Require(input != Invalid, "Open NUL standard input");
            IntPtr size = IntPtr.Zero;
            InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
            attributes = Marshal.AllocHGlobal(size);
            Require(InitializeProcThreadAttributeList(attributes, 1, 0, ref size), "Initialize explicit inherited-handle list");
            attributesInitialized = true;
            inherited = Marshal.AllocHGlobal(IntPtr.Size * 3);
            Marshal.WriteIntPtr(inherited, 0, input);
            Marshal.WriteIntPtr(inherited, IntPtr.Size, stdoutWrite);
            Marshal.WriteIntPtr(inherited, IntPtr.Size * 2, stderrWrite);
            Require(UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x20002), inherited,
                new IntPtr(IntPtr.Size * 3), IntPtr.Zero, IntPtr.Zero), "Set explicit inherited handles");
            StartupInfoEx startup = new StartupInfoEx();
            startup.Startup.Size = Marshal.SizeOf(typeof(StartupInfoEx));
            startup.Startup.Flags = StdHandles;
            startup.Startup.Input = input; startup.Startup.Output = stdoutWrite; startup.Startup.Error = stderrWrite;
            startup.Attributes = attributes;
            Require(CreateProcessW(file, new StringBuilder(Quote(file) + (String.IsNullOrEmpty(arguments) ? "" : " " + arguments)),
                IntPtr.Zero, IntPtr.Zero, true, Suspended | NoWindow | ExtendedStartup, IntPtr.Zero, workingdir, ref startup, out process), "Create suspended local process");
            result.ProcessId = process.ProcessId;
            Require(AssignProcessToJobObject(job, process.Process), "Assign suspended process to owned job");
            assigned = true;
            Close(ref stdoutWrite); Close(ref stderrWrite); Close(ref input);
            stdout = new Drain(stdoutRead); stdoutRead = IntPtr.Zero;
            stderr = new Drain(stderrRead); stderrRead = IntPtr.Zero;
            Require(ResumeThread(process.Thread) != InfiniteFailure, "Resume owned process");
            uint waited = WaitForSingleObject(process.Process, checked((uint)timeoutseconds * 1000));
            Require(waited == WaitObject || waited == WaitTimeout, "Wait for owned root process");
            result.TimedOut = waited == WaitTimeout;
            if (!result.TimedOut) {
                uint code; Require(GetExitCodeProcess(process.Process, out code), "Read root exit code");
                result.ExitCode = unchecked((int)code);
            }
        } catch (Exception error) { failure = error; }
        finally {
            // The held handles identify this exact job/root; never rediscover by PID.
            try {
                if (!assigned && process.Process != IntPtr.Zero) {
                    Require(TerminateProcess(process.Process, 1), "Terminate unassigned suspended process");
                }
                if (job != IntPtr.Zero) {
                    Require(TerminateJobObject(job, result.TimedOut ? 1460u : 1u), "Terminate residual owned job processes");
                    VerifyEmpty(job);
                }
                if (process.Process != IntPtr.Zero) {
                    Require(WaitForSingleObject(process.Process, 10000) == WaitObject, "Reap owned root process");
                    if (result.TimedOut) {
                        uint code; Require(GetExitCodeProcess(process.Process, out code), "Read terminated root exit code");
                        result.ExitCode = unchecked((int)code);
                    }
                }
                result.CleanupVerified = true;
            } catch (Exception error) { failure = failure == null ? error : new AggregateException(failure, error); }
            Close(ref stdoutWrite); Close(ref stderrWrite); Close(ref input);
            // Closing the job remains a final kill fence even if explicit cleanup failed.
            Close(ref job);
            try {
                if (stdout != null && !stdout.Completion.Wait(10000)) throw new TimeoutException("Stdout drain did not finish after owned job cleanup.");
                if (stderr != null && !stderr.Completion.Wait(10000)) throw new TimeoutException("Stderr drain did not finish after owned job cleanup.");
            } catch (Exception error) { failure = failure == null ? error : new AggregateException(failure, error); }
            if (stdout != null) result.Stdout = stdout.Snapshot();
            if (stderr != null) result.Stderr = stderr.Snapshot();
            if (stdout != null && stdout.Completion.IsCompleted) stdout.Dispose();
            if (stderr != null && stderr.Completion.IsCompleted) stderr.Dispose();
            Close(ref stdoutRead); Close(ref stderrRead);
            Close(ref process.Thread); Close(ref process.Process);
            if (attributesInitialized) DeleteProcThreadAttributeList(attributes);
            if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
            if (inherited != IntPtr.Zero) Marshal.FreeHGlobal(inherited);
            result.ElapsedMilliseconds = elapsed.ElapsedMilliseconds;
        }
        if (failure != null) {
            InvalidOperationException error = new InvalidOperationException("Local process execution or verified cleanup failed.", failure);
            error.Data["Result"] = result;
            error.Data["Stdout"] = result.Stdout;
            error.Data["Stderr"] = result.Stderr;
            throw error;
        }
        return result;
    }
}
