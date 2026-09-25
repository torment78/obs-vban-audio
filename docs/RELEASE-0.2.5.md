# VBAN Stream 0.2.5 — pre-release

A refreshed installer and clearer incoming-stream information for VBAN Stream on Windows x64.

## Changes

- Dark navy installer and uninstaller matching VBAN Plug, with VBAN Stream artwork, the original plugin icon, ElkaSoft branding and a Donate button.
- The installer retains the standard/portable OBS choices, folder browsing, validation and settings-preservation behavior.
- Every incoming slot now has a boxed **Channels** count and **Input format**, showing the actual incoming audio before OBS downmixing.
- **Receiving: N / 8 streams** counts live streams, not channels or duplicate OBS source instances. Waiting, disabled, errored and timed-out streams show a dash instead of stale values.

Each named stream accepts 1–8 channels automatically. One source remains one mixer fader. For eight-channel OBS output, select 7.1 and a compatible output format; Stereo downmixes incoming audio. Both monitor returns remain stereo, with independently selectable PCM16 or PCM24.

## Validation

Built with Visual Studio 2026. All 12 core/OBS tests passed on OBS 31.1.1, followed by the monitor-return test and all eight incoming-channel cases on OBS 32.2.1. Tests include live channel/PCM changes, input indicators, timeouts and native OBS meter lanes. All 53 standard/portable installer checks passed. The installer and both ZIP layouts contain the same tested plugin DLL.

## Downloads

- **vban-stream-0.2.5-windows-x64-setup.exe** — recommended; dark installer for standard or portable OBS.
- **vban-stream-0.2.5-obs-root.zip** — merge the obs-plugins and data folders into the selected OBS root; supports installed and portable OBS.
- **vban-stream-0.2.5-windows-x64.zip** — ProgramData plugin layout for standard OBS.
- **VBAN-INSTALL.txt** — installation and usage guide.
- **VBAN-Stream-artwork.zip** — all eight PNG banners and matching JPEGs under 1 MB each.
- **social-preview.jpg** — horizontal social preview image.
- **SHA256SUMS.txt** — checksums for the downloads.

Close OBS before updating and keep only one plugin copy installed. The internal DLL/data folder name remains `obs-vban-audio` for compatibility with existing scenes and settings.

Windows x64 only; minimum supported OBS version 31.1.1. This is a pre-release for testing.

[ElkaSoft](https://elkasoft.xyz/) · [Discord support](https://discord.gg/AAhxYKzmkz) · [Report a bug](https://github.com/torment78/vban-stream/issues) · [Donate on Ko-fi](https://ko-fi.com/msffixit)
