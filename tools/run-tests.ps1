param([ValidateSet('RelWithDebInfo','Release','Debug')][string]$Configuration = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -prerelease -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio 2026 with C++ tools is required.' }
$cmakeBin = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$obsConfiguration = if ($Configuration -eq 'Debug') { 'Debug' } else { 'Release' }
$obsBin = Join-Path $repoRoot ".deps\obs-studio-31.1.1\build_x64\rundir\$obsConfiguration\bin\64bit"
$depsBin = Join-Path $repoRoot '.deps\obs-deps-2025-07-11-x64\bin'
$qtBin = Join-Path $repoRoot '.deps\obs-deps-qt6-2025-07-11-x64\bin'
$originalPath = $env:PATH
$originalQt = $env:QT_PLUGIN_PATH
try {
    $env:PATH = "$obsBin;$depsBin;$qtBin;$originalPath"
    $env:QT_PLUGIN_PATH = Join-Path $repoRoot '.deps\obs-deps-qt6-2025-07-11-x64\plugins'
    & (Join-Path $cmakeBin 'ctest.exe') --test-dir (Join-Path $repoRoot 'build_x64') -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
} finally {
    $env:PATH = $originalPath
    $env:QT_PLUGIN_PATH = $originalQt
}
