# Audio review — 0.2.2

Reviewed 2026-09-20. Scope: all plugin source files, packet parsing/encoding,
receive queues, monitor capture and timing, socket routing, configuration,
source lifetime, shutdown, build scripts, packaging and tests.

## Confirmed return defects and changes

- The old MonitorClock snapped timestamps within 1 ms, then abruptly accepted a
  new position beyond that threshold. A deterministic -500 ppm source clock
  produced 240 overlapping frames over 2,000 256-frame blocks (about 10.7 seconds
  at 48 kHz). This can cut the waveform even when no UDP packet is lost.
  The regression failed before the fix. Small differences now adjust block
  length by at most one frame, interpolating with the preceding sample and
  maintaining continuous output positions. Unadjusted PCM passes through exactly.
  Large timestamp jumps still explicitly resynchronize and are counted.
- Foreign media clocks previously waited until 100 ms of skew before correcting
  their arrival-rate difference. That exceeds the default return deadline. A
  separate 128-second simulation reproduced 100.042 ms of skew. A one-second
  filtered arrival estimate now makes gradual corrections after 5 ms of skew;
  with +/-1,000 ppm drift and 0–1.5 ms callback jitter, maximum measured skew is
  6.771 ms and output positions remain continuous.
- A 30 ms return deadline rejects audio arriving 45 ms late. Both a deterministic
  timeline test and actual OBS source callbacks cover this case. The default is
  now 60 ms, with a saved 20–200 ms setting. Increasing it adds monitoring latency.
- Capture reserve doubles from 64 to 128 preallocated blocks per monitored source.
  Each block holds up to 256 frames of eight float planes, about 1 MiB per source.
  At 48 kHz this holds up to 683 ms with full blocks, about 640 ms for 10 ms
  callbacks split into 256+224 frames, or 341 ms with 128-frame callbacks.
  Capacity depends on callback size and sample rate; it is not a fixed latency.
- The return worker requests MMCSS Pro Audio scheduling using a scoped handle.
  It falls back to ordinary scheduling if the service is unavailable, reports
  the result, and reverts registration when the worker exits. No system-wide
  scheduling, registry, firewall, or timer-resolution changes are made.
- Diagnostics distinguish clipping, non-finite source PCM, clock corrections,
  clock jumps, late audio, capture overflow, queue peak, socket errors and send
  timing gaps. They do not pretend to measure packets at the remote receiver.

## Other review findings

The receiver retains a 1 MiB requested socket buffer, bounded per-stream queues,
a 30 ms jitter target and approximately 10 ms output. These receive algorithms
are unchanged in this release. Equal-sized missing input packets are replaced
with silence; longer gaps/underruns resynchronize. That can be audible and cannot
be repaired by increasing a downstream return buffer. The requested receive
socket-buffer size is not currently exposed as a measured OS capacity.

No additional packing, PCM24 sign-extension, endian or fan-out defect was found
in the reviewed paths. Parser tests reject malformed packet lengths and invalid
formats. Source capture remains free of allocation, network calls and blocking
queue waits. Ring storage is allocated on the worker's heap. All timeline
conversion and small drift interpolation also run on that worker.

The SDK bootstrap had combined two CMake definitions into one argument and used
extra embedded quotes around prefix paths. Those arguments are now passed
separately; scripting is disabled with its actual OBS option name. The plugin
continues to build through Visual Studio 2026.

## Validation

All four CTest suites pass, plus the separate plugin integration host using the
official OBS 32.2.1 / Qt 6.11.1 runtime. The normal host uses OBS 31.1.1 / Qt 6.8.3.

- Existing parser/malformed-input, eight-input UDP, buffer, reordering, loss,
  duplicate/wraparound, save rollback, source properties/naming and shutdown tests.
- Return PCM24 boundaries, both actual UDP destinations, independent counters,
  source-interface selection, unavailable-adapter rollback, and stale status.
- 200,000 concurrent ring entries with ordering/wrap checks.
- Two minutes of simulated 1 kHz audio in each clock direction (+/-1,000 ppm),
  at 44.1, 48, 96 and 192 kHz. Output positions remain contiguous, long-term
  timestamp error stays within three frames, and adjacent waveform steps stay
  below 0.04 at 0.2 peak amplitude. This is simulated time, not a hardware soak.
- Reproduction of 30 ms deadline loss and a passing 60 ms case with 45 ms callbacks.
- Eight monitored sources, two seconds of actual OBS capture/UDP output, checking
  packet sequence continuity and every sample of the final 100 packets.
- Another actual OBS run with all eight sources arriving 45 ms late, checking
  every sample of the final 100 packets at both destinations.
- Filters, gain, OBS-version mute behavior, both monitoring modes, 44.1 kHz mono,
  AV sync offsets, deactivation/removal, repeated source churn and active shutdown.
- Old settings default to 60 ms; edits persist without changing the eight inputs.

## What is still a target-PC test

These tests do not establish the cause of every click in a particular LAN or
VoiceMeeter device. The earlier saved packet capture was incomplete and cannot
support a reliable packet-loss percentage. No live capture was restarted.

Real VoiceMeeter reception/listening, full OBS scene switching, long hardware
soaks, driver/DPC stalls, VPN/router behavior and receiver jitter settings still
need validation on the actual streaming/receiving PCs. No finite buffer guarantees
click-free playback under arbitrary stalls or lost input audio.

This return reconstructs source monitoring through public OBS APIs. It does not
capture the private final WASAPI device buffer. It does not guarantee sample-
identical timing with physical headphones, and its small linear drift correction
is not a general-purpose high-quality asynchronous sample-rate converter.

Reference: [Microsoft MMCSS](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service).
