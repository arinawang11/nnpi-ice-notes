# Enumerate every CDM child control with its ID, class and text. Used to check
# that the IDs in results.ps1 still match the running CDM build.
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W4 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
}
"@
$p = Get-Process DiskMark64 | Select-Object -First 1
$h = $p.MainWindowHandle
$rows = New-Object System.Collections.ArrayList
$cb = [W4+EnumProc]{
  param($ch, $l)
  $sb = New-Object System.Text.StringBuilder 512
  [void][W4]::GetWindowTextW($ch, $sb, 512)
  $cn = New-Object System.Text.StringBuilder 128
  [void][W4]::GetClassNameW($ch, $cn, 128)
  if ($sb.Length -gt 0) { [void]$rows.Add(("{0}`t{1}`t{2}" -f [W4]::GetDlgCtrlID($ch), $cn.ToString(), $sb.ToString())) }
  return $true
}
[void][W4]::EnumChildWindows($h, $cb, [IntPtr]::Zero)
$rows | ForEach-Object { $_ }
