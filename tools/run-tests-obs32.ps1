param([ValidateSet('RelWithDebInfo','Release','Debug')][string]$Configuration = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$runtime = Join-Path $repoRoot '.deps\obs-runtime-32.2.1\bin\64bit'
if (-not (Test-Path -LiteralPath (Join-Path $runtime 'obs.dll'))) { throw 'The optional official OBS 32.2.1 runtime is not present under .deps.' }
$originalPath = $env:PATH
$originalQt = $env:QT_PLUGIN_PATH
try {
    $env:PATH = "$runtime;$originalPath"
    $env:QT_PLUGIN_PATH = $runtime
    & (Join-Path $repoRoot "build_x64\$Configuration\obs-return-smoke32.exe") (Join-Path $repoRoot "build_x64\$Configuration\obs-vban-audio.dll") (Join-Path $repoRoot 'data') (Join-Path $repoRoot 'build_x64\return-smoke32-config')
    if ($LASTEXITCODE -ne 0) { throw "OBS 32.2.1 return smoke failed ($LASTEXITCODE)." }
    foreach ($channels in 1..8) {
        & (Join-Path $repoRoot "build_x64\$Configuration\obs-multichannel32.exe") (Join-Path $repoRoot "build_x64\$Configuration\obs-vban-audio.dll") (Join-Path $repoRoot 'data') (Join-Path $repoRoot "build_x64\multichannel32-$channels-config") $channels
        if ($LASTEXITCODE -ne 0) { throw "OBS 32.2.1 $channels-channel receive test failed ($LASTEXITCODE)." }
    }

} finally {
    $env:PATH = $originalPath
    $env:QT_PLUGIN_PATH = $originalQt
}
