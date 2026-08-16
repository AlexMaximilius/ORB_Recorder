# install.ps1 -- add ORB_Recorder Orb to the Start Menu.
#
# Creates a shortcut at:
#   %APPDATA%\Microsoft\Windows\Start Menu\Programs\ORB_Recorder Orb.lnk
# pointing at orb_recorder.exe (in this same folder), with the orb icon.
#
# Run:  powershell -ExecutionPolicy Bypass -File .\install.ps1
# Undo: .\uninstall.ps1

$ErrorActionPreference = "Stop"

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$exePath  = Join-Path $here "orb_recorder.exe"
$icoPath  = Join-Path $here "orb.ico"

if (-not (Test-Path $exePath)) {
    Write-Error "orb_recorder.exe not found at $exePath -- run .\build.bat first."
}

$startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$lnkPath   = Join-Path $startMenu "ORB_Recorder Orb.lnk"

$wsh = New-Object -ComObject WScript.Shell
$sc  = $wsh.CreateShortcut($lnkPath)
$sc.TargetPath       = $exePath
$sc.WorkingDirectory = $here
$sc.IconLocation     = "$icoPath,0"
$sc.Description      = "ORB_Recorder - always-on-top screen recorder"
$sc.WindowStyle      = 7      # Minimized (irrelevant for a windowless app, but tidy)
$sc.Save()

Write-Host "Installed: $lnkPath"
Write-Host "Launch via Start Menu -> 'ORB_Recorder Orb'"
Write-Host ""
Write-Host "Log file: $env:LOCALAPPDATA\ORB_Recorder\log.txt"
