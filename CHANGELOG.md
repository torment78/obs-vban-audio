# Changelog

## 0.2.3

- Add an independent PCM 16-bit / PCM 24-bit dropdown to each return, before its destination fields.
- Preserve PCM 24-bit for older configurations and save both format choices.
- Keep sample-rate handling unchanged (48 kHz returns when OBS is set to 48 kHz).
- Validate mixed-format UDP payloads and live format changes in OBS 31.1.1 and 32.2.1.

## 0.2.2

- Add an EXE installer with standard/portable choices, OBS folder validation and scoped uninstall support (same plugin DLL).

- Fix return audio gaps/overlaps caused by gradual source-clock drift.
- Add a 20–200 ms return buffer, defaulting to 60 ms.
- Double per-source capture queue capacity and request Windows audio scheduling.
- Add clipping, invalid-sample, clock and queue-peak diagnostics.
- Add a project icon and optional Ko-fi donation button.
- Expand delayed-callback, waveform and OBS compatibility tests.
- Correct SDK bootstrap argument handling and publish the Windows source/builds.

## 0.2.1

- Select an active local IPv4 adapter for both return streams.
- Show actual socket endpoints, sender timing and capture/late-audio counters.

## 0.2.0

- Add two independent destinations carrying the shared OBS monitoring mix.

## 0.1.1

- Keep the stream selector open during selection.
- Use friendly stream labels for automatically named OBS mixer sources.

## 0.1.0

- Eight shared VBAN audio input slots and native OBS sources/settings.
