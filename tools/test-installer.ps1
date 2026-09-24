param([string]$SetupPath = '')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'buildspec.json') -Raw | ConvertFrom-Json).version
if (-not $SetupPath) { $SetupPath = Join-Path $repoRoot "dist\vban-stream-$version-windows-x64-setup.exe" }
$runtime = Join-Path $repoRoot '.deps\obs-runtime-32.2.1'
$expectedDll = Join-Path $repoRoot 'build_x64\RelWithDebInfo\obs-vban-audio.dll'
if (-not (Test-Path -LiteralPath $SetupPath)) { throw 'Build the installer first.' }
if (-not (Test-Path -LiteralPath (Join-Path $runtime 'bin\64bit\obs64.exe'))) { throw 'The isolated OBS 32.2.1 test runtime is required.' }
$testRoot = Join-Path $repoRoot ('build_installer\test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$regKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{CBB7B1A9-2B1D-4AA5-8494-82CBA4EA194C}_is1'
if (Test-Path -LiteralPath $regKey) { throw 'Existing current-user installer registration found; refusing to change it during tests.' }
$checks = 0
function Assert-Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Run-Setup([string]$Mode, [string]$Root, [string]$Label, [bool]$ExpectSuccess) {
    $log = Join-Path $testRoot "$Label.log"
    $installerArguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-','/CURRENTUSER',"/MODE=$Mode",('/OBSROOT="' + $Root + '"'),('/LOG="' + $log + '"'))
    $process = Start-Process -FilePath $SetupPath -ArgumentList $installerArguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(45000)) { throw "Installer timed out. Process $($process.Id), log: $log" }
    if ($ExpectSuccess) {
        Assert-Check ($process.ExitCode -eq 0) "Installer failed ($($process.ExitCode)): $log"
    } else {
        Assert-Check ($process.ExitCode -ne 0) "Invalid install unexpectedly succeeded: $Label"
    }
}
function Run-Uninstall([string]$Root, [string]$Label, [bool]$ExpectSuccess) {
    $resolvedRoot = [IO.Path]::GetFullPath($Root)
    if (-not $resolvedRoot.StartsWith([IO.Path]::GetFullPath($testRoot) + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Uninstall target is outside this isolated test run.'
    }
    $uninstaller = Join-Path $resolvedRoot 'data\obs-plugins\obs-vban-audio\unins000.exe'
    $log = Join-Path $testRoot "$Label.log"
    $process = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="' + $log + '"')) -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(45000)) { throw "Uninstaller timed out: $log" }
    Assert-Check (($process.ExitCode -eq 0) -eq $ExpectSuccess) "Unexpected uninstaller result $($process.ExitCode): $log"
}
function Make-Fixture([string]$Name) {
    $root = Join-Path $testRoot $Name
    foreach ($relative in @('bin\64bit','data\obs-studio','obs-plugins\64bit','config\obs-studio\basic\scenes')) {
        New-Item -ItemType Directory -Path (Join-Path $root $relative) -Force | Out-Null
    }
    foreach ($file in @('obs64.exe','obs.dll')) {
        Copy-Item -LiteralPath (Join-Path $runtime "bin\64bit\$file") -Destination (Join-Path $root "bin\64bit\$file")
    }
    [IO.File]::WriteAllText((Join-Path $root 'config\obs-studio\basic\scenes\keep.json'), '{"keep":"scenes"}')
    [IO.File]::WriteAllText((Join-Path $root 'obs-plugins\64bit\another-plugin.dll'), 'unrelated plugin')
    [IO.File]::WriteAllText((Join-Path $root 'data\obs-studio\keep.txt'), 'OBS core data')
    return $root
}
$invalid = Join-Path $testRoot 'Not OBS'
New-Item -ItemType Directory -Path $invalid | Out-Null
Run-Setup portable $invalid 'reject-non-obs' $false
Assert-Check (-not (Test-Path -LiteralPath (Join-Path $invalid 'obs-plugins'))) 'Invalid folder was modified.'
$fixture = Make-Fixture 'Wrong architecture'
Copy-Item -LiteralPath $SetupPath -Destination (Join-Path $fixture 'bin\64bit\obs64.exe') -Force
Run-Setup portable $fixture 'reject-non-x64' $false
Run-Setup typo $invalid 'reject-unknown-mode' $false
foreach ($mode in @('portable','standard')) {
    $root = Make-Fixture "$mode OBS with spaces"
    $dll = Join-Path $root 'obs-plugins\64bit\obs-vban-audio.dll'
    $dataDir = Join-Path $root 'data\obs-plugins\obs-vban-audio'
    Run-Setup $mode (Join-Path $root 'bin\64bit') "$mode-reject-bin-folder" $false
    Run-Setup $mode $root "$mode-install" $true
    try {
        Assert-Check ((Get-FileHash -LiteralPath $dll).Hash -eq (Get-FileHash -LiteralPath $expectedDll).Hash) "$mode DLL differs from tested release."
        foreach ($asset in @('locale\en-US.ini','vban-audio.png','LICENSE.txt','VBAN-INSTALL.txt','unins000.exe')) {
            Assert-Check (Test-Path -LiteralPath (Join-Path $dataDir $asset)) "$mode missing $asset"
        }
        Assert-Check ((Test-Path -LiteralPath $regKey) -eq ($mode -eq 'standard')) "$mode uninstall registration is incorrect."
        if ($mode -eq 'standard') {
            Assert-Check ((Get-ItemProperty -LiteralPath $regKey).InstallLocation.TrimEnd('\') -eq $root) 'Wrong registered uninstall target.'
        }
        [IO.File]::WriteAllText((Join-Path $dataDir 'user-added.txt'), 'keep custom file')
        Run-Setup $mode $root "$mode-update" $true
        Assert-Check ((Get-FileHash -LiteralPath $dll).Hash -eq (Get-FileHash -LiteralPath $expectedDll).Hash) "$mode update changed payload."
        Assert-Check ((Get-ChildItem -LiteralPath $dataDir -Filter 'unins*.exe').Count -eq 1) 'Update created duplicate uninstallers.'
        if ($mode -eq 'portable') {
            $moved = Join-Path $testRoot 'Moved portable OBS'
            Copy-Item -LiteralPath $root -Destination $moved -Recurse
            Run-Uninstall $moved 'reject-moved-uninstaller' $false
            Assert-Check (Test-Path -LiteralPath $dll) 'Moved uninstaller changed original installation.'
            Run-Setup portable $moved 'moved-portable-reinstall' $true
            Run-Uninstall $moved 'moved-portable-uninstall' $true
            Assert-Check (Test-Path -LiteralPath $dll) 'Reinstalled moved copy removed files from the original OBS folder.'
            Assert-Check (-not (Test-Path -LiteralPath (Join-Path $moved 'obs-plugins\64bit\obs-vban-audio.dll'))) 'Moved portable uninstall left its own DLL.'
        }
    } finally {
        if (Test-Path -LiteralPath (Join-Path $dataDir 'unins000.exe')) {
            Run-Uninstall $root "$mode-uninstall" $true
        }
    }
    Assert-Check (-not (Test-Path -LiteralPath $dll)) "$mode uninstall left the plugin DLL."
    Assert-Check (-not (Test-Path -LiteralPath (Join-Path $dataDir 'locale\en-US.ini'))) "$mode uninstall left the installed locale."
    Assert-Check ((Get-Content -LiteralPath (Join-Path $dataDir 'user-added.txt') -Raw) -eq 'keep custom file') 'Uninstall removed a file it did not install.'
    Assert-Check ((Get-Content -LiteralPath (Join-Path $root 'config\obs-studio\basic\scenes\keep.json') -Raw) -eq '{"keep":"scenes"}') 'Scenes changed.'
    Assert-Check ((Get-Content -LiteralPath (Join-Path $root 'obs-plugins\64bit\another-plugin.dll') -Raw) -eq 'unrelated plugin') 'Unrelated plugin changed.'
    Assert-Check ((Get-Content -LiteralPath (Join-Path $root 'data\obs-studio\keep.txt') -Raw) -eq 'OBS core data') 'OBS data changed.'
    Assert-Check ((Get-FileHash -LiteralPath (Join-Path $root 'bin\64bit\obs64.exe')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $runtime 'bin\64bit\obs64.exe')).Hash) 'OBS executable changed.'
    Assert-Check (-not (Test-Path -LiteralPath $regKey)) 'Uninstall registration was not cleaned up.'
}
Write-Output "PASS: $checks installer checks. Logs: $testRoot"
