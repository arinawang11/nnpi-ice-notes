# Click a CrystalDiskMark test button and wait for the DiskSpd child.
# 1006 = RND4K only, 1003 = All. Button IDs are for CDM 9.0.3.
param([int]$ButtonId = 1006)

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WT {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hDlg, int nIDDlgItem);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageSB(IntPtr hWnd, uint msg, IntPtr wp, StringBuilder lp);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hWnd);
}
"@

$p = Get-Process DiskMark64 | Select-Object -First 1
$h = $p.MainWindowHandle
$BM_CLICK = 0x00F5

function Combo-Text($id) {
    $c = [WT]::GetDlgItem($h, $id)
    $i = [int][WT]::SendMessage($c, 0x0147, [IntPtr]::Zero, [IntPtr]::Zero)
    $sb = New-Object System.Text.StringBuilder 260
    [void][WT]::SendMessageSB($c, 0x0148, [IntPtr]$i, $sb)
    return $sb.ToString()
}

$drive = Combo-Text 1027
$size = Combo-Text 1028
Write-Output ("drive=" + $drive + " size=" + $size)
if (-not $drive.StartsWith("G:")) { Write-Output "WRONG DRIVE"; exit 2 }
if ($size -ne "256MiB") { Write-Output "WRONG SIZE"; exit 3 }

$btn = [WT]::GetDlgItem($h, $ButtonId)
Write-Output ("button " + $ButtonId + " enabled=" + [WT]::IsWindowEnabled($btn))

for ($try = 1; $try -le 3; $try++) {
    [void][WT]::SendMessage($btn, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 1200
    $d = Get-Process DiskSpd64 -ErrorAction SilentlyContinue
    if ($d) { Write-Output ("started attempt " + $try); break }
    Write-Output ("no diskspd after attempt " + $try)
}

$d = Get-Process DiskSpd64 -ErrorAction SilentlyContinue
if (-not $d) { Write-Output "NOT STARTED"; exit 4 }

# Wait for the child to exit (CDM spawns one diskspd per pass).
$deadline = (Get-Date).AddSeconds(180)
while ((Get-Process DiskSpd64 -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 2
Write-Output "FINISHED"
