param(
    [string]$InnoCompiler = '',
    [ValidateSet('RelWithDebInfo','Release')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $InnoCompiler) { $InnoCompiler = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Inno Setup 6\ISCC.exe' }
$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'buildspec.json') -Raw | ConvertFrom-Json).version
$releaseZip = Join-Path $repoRoot "dist\obs-vban-audio-$version-obs-root.zip"
$builtDll = Join-Path $repoRoot "build_x64\$Configuration\obs-vban-audio.dll"
if (-not (Test-Path -LiteralPath $InnoCompiler)) { throw 'Install Inno Setup 6, or specify -InnoCompiler.' }
if (-not (Test-Path -LiteralPath $releaseZip)) { throw 'Run tools/build.ps1 -Package with Visual Studio 2026 first.' }
if (-not (Test-Path -LiteralPath $builtDll)) { throw 'The matching Visual Studio plugin build is required.' }
$stage = Join-Path $repoRoot ('build_installer\payload-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($releaseZip)
try {
    foreach ($relativePath in @(
        'obs-plugins/64bit/obs-vban-audio.dll',
        'data/obs-plugins/obs-vban-audio/locale/en-US.ini',
        'data/obs-plugins/obs-vban-audio/vban-audio.png'
    )) {
        $entry = @($archive.Entries | Where-Object { $_.FullName.Replace('\','/') -eq $relativePath })
        if ($entry.Count -ne 1) { throw "Expected exactly one $relativePath in the release ZIP." }
        $destination = Join-Path $stage $relativePath
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry[0], $destination, $false)
    }
} finally { $archive.Dispose() }
$payloadDll = Join-Path $stage 'obs-plugins\64bit\obs-vban-audio.dll'
$payloadHash = (Get-FileHash -LiteralPath $payloadDll -Algorithm SHA256).Hash
if ($payloadHash -ne (Get-FileHash -LiteralPath $builtDll -Algorithm SHA256).Hash) {
    throw 'The release ZIP DLL does not match the Visual Studio build. Do not publish mixed binaries.'
}
$script = Join-Path $repoRoot 'installer\obs-vban-audio.iss'
$output = Join-Path $repoRoot 'dist'
& $InnoCompiler "/DAppVersion=$version" "/DPayloadDir=$stage" "/DOutputPath=$output" $script
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
$setup = Join-Path $output "obs-vban-audio-$version-windows-x64-setup.exe"
Get-FileHash -LiteralPath $setup -Algorithm SHA256 | Format-List
Write-Output "Payload DLL SHA256: $payloadHash"
Write-Output "Installer: $setup"
