# Windows installer

The EXE is the dark installer package for the 0.2.5 pre-release. It contains the
same tested plugin DLL as the ZIP downloads.

## Build

Build the plugin and ZIPs with Visual Studio 2026 first:

```powershell
.\tools\build.ps1 -Package
.\tools\package-installer.ps1
```

The second command uses locally installed Inno Setup 6.6 or newer (validated with 6.6.1).
It extracts an explicit list of payload files from the root ZIP and checks the
DLL SHA256 against the Visual Studio build before compiling the installer.
Use `-InnoCompiler` for a non-default compiler path.

Packaging also converts the existing plugin PNG into a multi-resolution Windows
ICO and embeds it in Setup/Uninstall. The Installed apps entry uses the uninstaller's
matching icon. All seven ICO frame payloads were verified in the compiled setup EXE.

Output: `dist\vban-stream-0.2.5-windows-x64-setup.exe`.

## Appearance

Setup and Uninstall use the same `modern dark polar includetitlebar` theme as
VBAN Plug, with a 120% wizard size. The welcome and finish pages use VBAN Stream's
existing circuit portrait, fitted without cropping or changing its aspect ratio.
The original plugin icon appears in the header and executable, and the shared
ElkaSoft logo appears on the welcome and finish pages. A Donate button opens
https://ko-fi.com/msffixit as the original Windows user.

The standard/portable choices, folder browser, validation, payload paths and
settings-preservation behavior are unchanged.

For an installer-only preview using a previously validated release DLL, both
packaging and tests accept `-ExpectedDllPath`. Verify that DLL against the
published release checksum before using it. Packaging still checks the root ZIP
against that reference, instead of implicitly including the current development
build. Use `-OutputDirectory` to keep preview installers separate from release
assets. Defaults still compare against the Visual Studio build and output to `dist`.

The 25 September 2026 dark preview is in `dist/installer-dark-preview` and uses
the original published 0.2.4 payload. It passed all 53 installer checks; logs are
under `build_installer/test-d82dac9bae8940bda1a8a4ba2ff4fcd1` and
`build_installer/dark-preview-tests.log`. Desktop visual inspection could not run:
the Computer Use runtime failed to initialize with `apply deny-read ACLs` on both
attempts. Visible layout, high-DPI appearance and interactive browsing remain
unverified. No GitHub release asset was replaced.

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

53 checks passed for the final 0.2.5 installer:

- Both standard and portable installation, matching DLL hash and required files.
- Repeated installation/update without duplicate uninstallers.
- Correct presence/absence of the Windows uninstall registration.
- Invalid mode, non-OBS folder, bin-subfolder selection and wrong architecture.
- Paths containing spaces.
- Uninstall leaves scenes, OBS core files, another plugin and a user-added file.
- An uninstaller copied with a moved OBS folder refuses the old paths.
- Reinstall/uninstall in the moved folder leaves the original OBS copy intact.

Final 0.2.5 logs are in `build_logs/0.2.5` and
`build_installer/test-fc55731231cd4b67a3834d90431c7036`.

The tested DLL SHA256 is
`13E2038DA48C07D7EE71B4415778B6B41B44259DEC014A219876E25899DA655E`.

The administrator consent dialog and writes to protected Program Files folders
are not exercised by these non-elevated tests. Desktop UI inspection was
unavailable because the computer-use runtime failed to initialize. Silent tests
exercise the same wizard validation and installation/uninstallation code, but do
not verify visible text layout or interactive folder browsing.

[Inno Setup privilege documentation](https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm)
and [script event documentation](https://jrsoftware.org/ishelp/topic_scriptevents.htm).
