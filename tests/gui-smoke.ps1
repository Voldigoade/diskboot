param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DiskBootUiTest {
    public delegate bool WindowCallback(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] public static extern bool EnumThreadWindows(uint id, WindowCallback callback, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr w, System.Text.StringBuilder c, int max);
    public static IntPtr FindOwned(int pid) {
        IntPtr found = IntPtr.Zero;
        foreach(System.Diagnostics.ProcessThread thread in System.Diagnostics.Process.GetProcessById(pid).Threads) {
            EnumThreadWindows((uint)thread.Id, (w, d) => {
                var name = new System.Text.StringBuilder(256);
                GetClassName(w, name, 256);
                if(name.ToString() == "DiskBootWindow") {found = w; return false;}
                return true;
            }, IntPtr.Zero);
            if(found != IntPtr.Zero) break;
        }
        return found;
    }
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint id);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr w);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
}
'@
$process = Start-Process -FilePath ([IO.Path]::GetFullPath($Executable)) -WindowStyle Hidden -PassThru
try {
    if (!$process.WaitForInputIdle(10000)) { throw 'GUI did not become responsive' }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $window = [DiskBootUiTest]::FindOwned($process.Id)
        [UInt32]$owner = 0
        [void][DiskBootUiTest]::GetWindowThreadProcessId($window, [ref]$owner)
        if ($window -ne [IntPtr]::Zero -and $owner -eq $process.Id) { break }
        if ($process.HasExited) { throw "GUI exited during startup with code $($process.ExitCode)" }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero -or $owner -ne $process.Id) {
        $process.Refresh()
        throw "Unexpected GUI window identity: handle=$window owner=$owner expected=$($process.Id) title=$($process.MainWindowTitle) main=$($process.MainWindowHandle)"
    }
    if (![DiskBootUiTest]::IsWindowEnabled([DiskBootUiTest]::GetDlgItem($window, 10))) { throw 'ISO picker is unavailable' }
    if ([DiskBootUiTest]::IsWindowEnabled([DiskBootUiTest]::GetDlgItem($window, 16))) { throw 'Next is enabled without an inspected ISO' }
    Write-Output 'PASS: native GUI starts, enables the ISO picker, and disables forward navigation without an ISO. This is not visual QA.'
} finally {
    if ($window -ne [IntPtr]::Zero) { [void][DiskBootUiTest]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
    if (!$process.WaitForExit(5000)) { $process.Kill() }
}
