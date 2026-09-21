# Windows installer

The EXE is an additional package for the existing 0.2.2 release. It contains the
same tested plugin DLL as the ZIP downloads; no audio-engine changes are made.

## Build

Build the plugin and ZIPs with Visual Studio 2026 first:

```powershell
.\tools\build.ps1 -Package
.\tools\package-installer.ps1
```

The second command uses locally installed Inno Setup 6 (validated with 6.6.1).
It extracts an explicit list of payload files from the root ZIP and checks the
DLL SHA256 against the Visual Studio build before compiling the installer.
Use `-InnoCompiler` for a non-default compiler path.

Output: `dist\obs-vban-audio-0.2.2-windows-x64-setup.exe`.

## Installation behavior

- Standard OBS is selected by default. Setup looks for OBS's installation path
  in the Windows registry, falling back to the usual Program Files folder.
  The user can browse to another installed OBS folder.
- Portable OBS starts with an empty folder selection and a Browse button.
- Both choices require an existing OBS root containing `bin\64bit\obs64.exe`,
  `bin\64bit\obs.dll`, `data` and `obs-plugins`. Setup checks that OBS is a 64-bit
  executable and refuses an invalid destination.
- Both choices use the OBS-root file layout. Standard mode additionally registers
  a Windows Installed apps entry. Portable mode keeps the uninstaller local.
- Setup requests administrator privileges by default so it can write protected
  folders. It neither launches OBS nor changes firewall, network or audio settings.
- A read-only Windows process query blocks installation/removal while OBS runs.
  Setup never automatically terminates OBS.
- Standard mode blocks if the usual ProgramData copy is present, avoiding a second
  plugin installation. Other manually renamed or unusual locations are not scanned.

The installed manifest consists only of the plugin DLL, its English locale,
project icon, license and installation guide, plus Inno Setup's uninstaller.
OBS executable/data files, scenes, profiles and plugin configuration are not
installation payloads and are not removed.

The uninstaller checks its current location before acting. After moving portable
OBS, rerun the installer for the new root first. The uninstall log is overwritten
on reinstall, so it records only that root, not paths from an earlier location.
When changing the payload list in a future release, review cleanup of obsolete
plugin-owned files explicitly.

## Validation

Run:

```powershell
.\tools\test-installer.ps1
```

Requires the optional OBS 32.2.1 runtime under `.deps\obs-runtime-32.2.1`.
The script runs the exact release EXE using `/CURRENTUSER` and explicit isolated
destinations under `build_installer`. It does not start OBS. It refuses to run
over an existing current-user uninstall registration and removes the temporary
registration through the uninstaller.

53 checks passed for the final 0.2.2 installer:

- Both standard and portable installation, matching DLL hash and required files.
- Repeated installation/update without duplicate uninstallers.
- Correct presence/absence of the Windows uninstall registration.
- Invalid mode, non-OBS folder, bin-subfolder selection and wrong architecture.
- Paths containing spaces.
- Uninstall leaves scenes, OBS core files, another plugin and a user-added file.
- An uninstaller copied with a moved OBS folder refuses the old paths.
- Reinstall/uninstall in the moved folder leaves the original OBS copy intact.

The tested DLL SHA256 is
`AD9F11DD406ACB3F332AF34A9AFA936FF017AE8142455C0C8F1E48752E070E68`.

The administrator consent dialog and writes to protected Program Files folders
are not exercised by these non-elevated tests. Desktop UI inspection was
unavailable because the computer-use runtime failed to initialize. Silent tests
exercise the same wizard validation and installation/uninstallation code, but do
not verify visible text layout or interactive folder browsing.

[Inno Setup privilege documentation](https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm)
and [script event documentation](https://jrsoftware.org/ishelp/topic_scriptevents.htm).
