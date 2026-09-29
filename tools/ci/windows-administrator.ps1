# Enter the installed setup DLL through the actual 64-bit ODBC Administrator.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.ComponentModel;
public static class SetupUiMessages {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    private delegate bool EnumCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    public static IntPtr VisibleDialog(int process) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter) {
            uint owner; Rect rect;
            GetWindowThreadProcessId(window, out owner);
            if (owner == process && IsWindowVisible(window) && GetWindowRect(window, out rect) &&
                rect.Right - rect.Left > 200 && rect.Bottom - rect.Top > 200) {
                found = window; return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr window, EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int size);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] private static extern IntPtr SendText(IntPtr window, uint message, IntPtr wp, string text);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr window);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern IntPtr OpenProcess(uint access, bool inherit, uint process);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, IntPtr size, uint allocation, uint protection);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] data, IntPtr size, out IntPtr written);
    [DllImport("kernel32.dll")] private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, IntPtr size, uint type);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    private static IntPtr Child(IntPtr parent, string wanted) {
        IntPtr found=IntPtr.Zero;
        EnumChildWindows(parent,delegate(IntPtr window,IntPtr parameter) {
            var name=new StringBuilder(128);GetClassName(window,name,name.Capacity);
            if(name.ToString()==wanted){found=window;return false;}return true;
        },IntPtr.Zero);
        return found;
    }
    public static int TabCount(IntPtr parent) {
        return SendMessage(Child(parent,"SysTabControl32"),0x1304,IntPtr.Zero,IntPtr.Zero).ToInt32();
    }
    public static void SelectDriver(IntPtr dialog,string name) {
        var list=Child(dialog,"SysListView32");
        if(list==IntPtr.Zero) {
            list=Child(dialog,"ListBox");
            if(list==IntPtr.Zero)throw new Exception("Native driver list not found.");
            var index=SendText(list,0x1A2,new IntPtr(-1),name);
            if(index.ToInt64()<0)throw new Exception("Packaged driver not listed in Administrator.");
            SendMessage(list,0x186,index,IntPtr.Zero);
            SendMessage(GetParent(list),0x111,new IntPtr(GetDlgCtrlID(list)|(1<<16)),list);
            return;
        }
        uint owner;GetWindowThreadProcessId(list,out owner);
        var process=OpenProcess(0x428,false,owner);if(process==IntPtr.Zero)throw new Win32Exception();
        var memory=IntPtr.Zero;
        try {
            memory=VirtualAllocEx(process,IntPtr.Zero,new IntPtr(512),0x3000,4);
            if(memory==IntPtr.Zero)throw new Win32Exception();
            // x64 LVFINDINFOW plus UTF-16 text, in memory owned by our launched app.
            var data=new byte[512];BitConverter.GetBytes(2u).CopyTo(data,0);
            BitConverter.GetBytes(memory.ToInt64()+128).CopyTo(data,8);
            Encoding.Unicode.GetBytes(name+"\0").CopyTo(data,128);IntPtr written;
            if(!WriteProcessMemory(process,memory,data,new IntPtr(data.Length),out written))throw new Win32Exception();
            var index=SendMessage(list,0x1053,new IntPtr(-1),memory);
            if(index.ToInt64()<0)throw new Exception("Packaged driver not listed in Administrator.");
            data=new byte[512];BitConverter.GetBytes(3u).CopyTo(data,12);BitConverter.GetBytes(3u).CopyTo(data,16);
            if(!WriteProcessMemory(process,memory,data,new IntPtr(data.Length),out written))throw new Win32Exception();
            if(SendMessage(list,0x102B,index,memory)==IntPtr.Zero)throw new Exception("Cannot select packaged driver.");
            SendMessage(list,0x1013,index,IntPtr.Zero);
        } finally {if(memory!=IntPtr.Zero)VirtualFreeEx(process,memory,IntPtr.Zero,0x8000);CloseHandle(process);}
    }
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
}
'@
$root = [System.Windows.Automation.AutomationElement]::RootElement
$scope = [System.Windows.Automation.TreeScope]::Descendants
function Named($Parent, [string]$Name) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
    $Parent.FindFirst($scope, $condition)
}
function Wait-Named($Parent, [string]$Name) {
    $deadline = (Get-Date).AddSeconds(20)
    do {
        $element = Named $Parent $Name
        if ($element) { return $element }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    Write-Host "UI tree while waiting for '$Name':"
    $Parent.FindAll($scope,[System.Windows.Automation.Condition]::TrueCondition) | Select-Object -First 80 | ForEach-Object {
        Write-Host ($_.Current.ControlType.ProgrammaticName + ': ' + $_.Current.Name)
    }
    $handle = [IntPtr]$Parent.Current.NativeWindowHandle
    if ($handle -ne [IntPtr]::Zero) {
        $rect = New-Object SetupUiMessages+Rect
        if ([SetupUiMessages]::GetWindowRect($handle,[ref]$rect) -and $rect.Right -gt $rect.Left -and $rect.Bottom -gt $rect.Top) {
            $bitmap = New-Object System.Drawing.Bitmap ($rect.Right-$rect.Left),($rect.Bottom-$rect.Top)
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            $dc = $graphics.GetHdc()
            [void][SetupUiMessages]::PrintWindow($handle,$dc,2)
            $graphics.ReleaseHdc($dc); $graphics.Dispose()
            $bitmap.Save("$env:RUNNER_TEMP/odbcpp-administrator-failure.png")
            $bitmap.Dispose()
        }
    }
    throw "Administrator UI element not found: $Name"
}
function Invoke-Button($Parent, [string]$Name) {
    $element = Wait-Named $Parent $Name
    # Post native button clicks so modal dialogs cannot block the automation client.
    $handle = [IntPtr]$element.Current.NativeWindowHandle
    if ($handle -eq [IntPtr]::Zero -or ![SetupUiMessages]::PostMessage($handle,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)) {
        throw "Cannot invoke native Administrator button: $Name"
    }
}
$process = Start-Process "$env:SystemRoot/System32/odbcad32.exe" -PassThru
try {
    $deadline = (Get-Date).AddSeconds(20)
    do {
        $handle = [SetupUiMessages]::VisibleDialog($process.Id)
        if ($handle -ne [IntPtr]::Zero) {
            $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
            break
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    if (!$window) { throw '64-bit ODBC Administrator did not open' }
    Write-Host ('Administrator window: ' + $window.Current.Name)
    # The fresh Administrator starts on User DSN. Generic accessibility proxies
    # expose the page label but not individual native tab/list items.
    [void](Wait-Named $window 'User Data Sources:')
    Invoke-Button $window 'Add...'
    $create = Wait-Named $root 'Create New Data Source'
    [SetupUiMessages]::SelectDriver([IntPtr]$create.Current.NativeWindowHandle,'ODBCPP PostgreSQL')
    Invoke-Button $create 'Finish'
    $setup = Wait-Named $root 'ODBCPP PostgreSQL Setup'
    if ([SetupUiMessages]::TabCount([IntPtr]$setup.Current.NativeWindowHandle) -ne 2) { throw 'Installed setup tabs are missing' }
    [void](Wait-Named $setup 'Data source name')
    Invoke-Button $setup 'Cancel'
    Write-Host 'Installed setup opened through 64-bit ODBC Administrator; both tabs found and Add canceled.'
} finally {
    if (!$process.HasExited) {
        [void]$process.CloseMainWindow()
        if (!$process.WaitForExit(5000)) { $process.Kill() }
    }
}
