# Enter the installed setup DLL through the actual 64-bit ODBC Administrator.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
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
    $tab = Wait-Named $window 'User DSN'
    $tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Invoke-Button $window 'Add...'
    $create = Wait-Named $root 'Create New Data Source'
    $driver = Wait-Named $create 'ODBCPP PostgreSQL'
    $driver.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Invoke-Button $create 'Finish'
    $setup = Wait-Named $root 'ODBCPP PostgreSQL Setup'
    [void](Wait-Named $setup 'Connection')
    [void](Wait-Named $setup 'Authentication')
    Invoke-Button $setup 'Cancel'
    Write-Host 'Installed setup opened through 64-bit ODBC Administrator; both tabs found and Add canceled.'
} finally {
    if (!$process.HasExited) {
        [void]$process.CloseMainWindow()
        if (!$process.WaitForExit(5000)) { $process.Kill() }
    }
}
