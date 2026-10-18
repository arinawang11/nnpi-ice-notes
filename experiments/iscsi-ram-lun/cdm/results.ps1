# Read the four result rows from CDM 9.0.3 by control ID.
# 1009-1012 are reads (SEQ8, SEQ1, RND4K Q32, RND4K Q1); 1014-1017 are writes.
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W3 {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hDlg, int nIDDlgItem);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="GetWindowTextW")] public static extern int GetWindowText(IntPtr hWnd, StringBuilder s, int n);
}
"@
$p = Get-Process DiskMark64 | Select-Object -First 1
$h = $p.MainWindowHandle
foreach ($id in 1009,1010,1011,1012,1014,1015,1016,1017) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][W3]::GetWindowText([W3]::GetDlgItem($h, $id), $sb, 512)
    Write-Output ("$id" + "`t" + $sb.ToString())
}
