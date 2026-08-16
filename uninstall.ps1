# uninstall.ps1 -- remove ORB_Recorder Orb from the Start Menu.
# (Leaves the exe + source folder alone; only removes the .lnk.)

$startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$lnkPath   = Join-Path $startMenu "ORB_Recorder Orb.lnk"

if (Test-Path $lnkPath) {
    Remove-Item $lnkPath -Force
    Write-Host "Removed: $lnkPath"
} else {
    Write-Host "Not installed: $lnkPath"
}
