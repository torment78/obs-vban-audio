param([ValidateSet('RelWithDebInfo','Release')][string]$Configuration = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$binary = Join-Path $repoRoot "build_x64\$Configuration\obs-vban-audio.dll"
if (-not (Test-Path -LiteralPath $binary)) { throw 'Build the plugin before packaging.' }
$stage = Join-Path $repoRoot 'dist\obs-root'
$pluginBin = Join-Path $stage 'obs-plugins\64bit'
$pluginData = Join-Path $stage 'data\obs-plugins\obs-vban-audio'
New-Item -ItemType Directory -Path $pluginBin,$pluginData -Force | Out-Null
Copy-Item -LiteralPath $binary -Destination $pluginBin -Force
New-Item -ItemType Directory -Path (Join-Path $pluginData 'locale') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'data\locale\en-US.ini') -Destination (Join-Path $pluginData 'locale\en-US.ini') -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'data\vban-audio.png') -Destination $pluginData -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $pluginData 'LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\INSTALL-OBS-ROOT.txt') -Destination (Join-Path $stage 'VBAN-INSTALL.txt') -Force
$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'buildspec.json') -Raw | ConvertFrom-Json).version
$zip = Join-Path $repoRoot "dist\obs-vban-audio-$version-obs-root.zip"
Compress-Archive -LiteralPath (Join-Path $stage 'obs-plugins'),(Join-Path $stage 'data'),(Join-Path $stage 'VBAN-INSTALL.txt') -DestinationPath $zip -Force
Write-Output "OBS-root package: $zip"
