# Builds VirtTilt (Release) using the CMake bundled with Visual Studio.
param([string]$Config = "Release", [switch]$Clean)
$ErrorActionPreference = "Stop"

$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

& $cmake -S $PSScriptRoot -B "$PSScriptRoot\build" -A x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# Stop any running instance from the build directory so MSBuild can overwrite the exe.
Get-Process VirtTilt -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "*\build\*" } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 200

$buildArgs = @("--build", "$PSScriptRoot\build", "--config", $Config)
if ($Clean) { $buildArgs += "--clean-first" }
& $cmake @buildArgs
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$exe = "$PSScriptRoot\build\$Config\VirtTilt.exe"
Write-Host "`nBuilt: $exe ($([math]::Round((Get-Item $exe).Length / 1KB)) KB)"
