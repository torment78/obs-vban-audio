param(
    [ValidateSet('RelWithDebInfo','Release','Debug')][string]$Configuration = 'RelWithDebInfo',
    [switch]$SkipTests,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2026 with Desktop development with C++.' }
$vsPath = & $vswhere -latest -prerelease -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio 2026 C++ tools were not found.' }
$cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'Add C++ CMake tools for Windows in Visual Studio Installer.' }
Push-Location $repoRoot
try {
    & $cmake --preset windows-x64 "-DCMAKE_GENERATOR_INSTANCE=$vsPath"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & $cmake --build build_x64 --config $Configuration --parallel 8
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio build failed.' }
    if (-not $SkipTests) {
        & (Join-Path $PSScriptRoot 'run-tests.ps1') -Configuration $Configuration
        if ($Configuration -ne 'Debug' -and (Test-Path -LiteralPath (Join-Path $repoRoot '.deps\obs-runtime-32.2.1\bin\64bit\obs.dll'))) {
            & (Join-Path $PSScriptRoot 'run-tests-obs32.ps1') -Configuration $Configuration
        }
    }
    if ($Package) {
        if ($Configuration -eq 'Debug') { throw 'Use RelWithDebInfo or Release for a distributable package.' }
        $stage = Join-Path $repoRoot 'dist\package'
        & $cmake --install build_x64 --config $Configuration --prefix $stage
        if ($LASTEXITCODE -ne 0) { throw 'Package staging failed.' }
        Copy-Item -LiteralPath README.md,LICENSE -Destination $stage -Force
        Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\INSTALL-OBS-ROOT.txt') -Destination (Join-Path $stage 'VBAN-INSTALL.txt') -Force
        $version = (Get-Content -LiteralPath buildspec.json -Raw | ConvertFrom-Json).version
        $zip = Join-Path $repoRoot "dist\vban-stream-$version-windows-x64.zip"
        Compress-Archive -LiteralPath (Join-Path $stage 'obs-vban-audio'),(Join-Path $stage 'README.md'),(Join-Path $stage 'LICENSE'),(Join-Path $stage 'VBAN-INSTALL.txt') -DestinationPath $zip -Force
        Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Format-List
        Write-Output "Package: $zip"
        & (Join-Path $PSScriptRoot 'package-obs-root.ps1') -Configuration $Configuration
    }
} finally { Pop-Location }
