# Builds VirtTilt (Release), installs it to %LOCALAPPDATA%\Programs\VirtTilt,
# registers autostart and launches it. Run this from your own terminal.
$ErrorActionPreference = "Stop"

& "$PSScriptRoot\build.ps1"
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$target = Join-Path $env:LOCALAPPDATA "Programs\VirtTilt"
$exe = Join-Path $target "VirtTilt.exe"

# Stop a running instance so the exe can be replaced.
Get-Process VirtTilt -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item "$PSScriptRoot\build\Release\VirtTilt.exe" $exe -Force

Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" `
    -Name "VirtTilt" -Value "`"$exe`""

Start-Process $exe
Write-Host "Installed to $exe and started. Autostart enabled."
