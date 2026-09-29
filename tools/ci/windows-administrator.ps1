# Enter the installed setup DLL through the actual 64-bit ODBC Administrator.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SetupUiMessages {
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
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
    do {
        $window = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
        if ($window) { break }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    if (!$window) { throw '64-bit ODBC Administrator did not open' }
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
