# Monitor return implementation (0.2.2)

The existing receiver.cpp, stream-buffer.cpp and vban-protocol.cpp processing is
unchanged. Config gains two optional return records; missing records default off.
The existing JSON version remains 1, with an optional returns array. Return socket
preparation and RX validation occur before QSaveFile commit. Rejected edits retain
the active configuration. Destinations are activated only after that commit.

## Verified OBS capture semantics

Inspected against the official OBS 32.2.1 sources and headers:

- [obs.h](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs.h):
  obs_source_audio_capture_t takes source, audio_data and muted; audio_monitoring
  is available as a source signal. Weak references and removal APIs are public.
- [obs-source.c](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/obs-source.c):
  process_audio converts raw source data into OBS's rate, layout and float-planar
  format before filter_async_audio. source_output_audio_data then invokes capture
  callbacks on that post-filter data. Capture timestamps retain the source clock,
  rather than exposing OBS's private adjusted program-buffer timestamp.
  The audio_monitoring signal is emitted BEFORE the source monitoring_type field
  changes; the implementation therefore uses calldata's type, not an immediate getter.
- [Windows monitoring](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/audio-monitoring/win32/wasapi-output.c):
  on_audio_playback checks active references, resamples/remixes and applies
  user_volume. In 32.2.0/32.2.1 it ignores the callback's muted argument.
  Verified 31.1.1, 32.0.0 and 32.1.0 honor that argument. Runtime version >=32.2
  selects the new behavior, matching the tested 32.2.1 host.
- OBS monitoring applies sync_offset to sources with video, except sources that
  are both async_unbuffered and async_decoupled. Pure audio monitoring does not
  apply this offset. Return handling follows those public settings.

No track tap, capture-device loopback, source filter insertion, private libobs
struct access or VoiceMeeter Remote DLL is used in the plugin.

## Threads and lifetime

Global source_create plus initial public input-source enumeration find audio
sources. Retained per-source signal handlers track monitoring, deactivate,
remove and destroy. A worker attaches capture only when a return is enabled and
the source is monitored. The atomic monitoring gate excludes an OFF source
immediately; epochs discard buffered data after OFF/removal/deactivation.

Tap objects hold weak source references. Strong references are held only around
OBS API calls. OBS clears/quiesces capture callbacks before emitting destroy.
Retained signal-handler references keep disconnect safe after source destruction.
Rapid source pointer reuse replaces the old dead entry. Registry locks are never
taken in capture callbacks, and OBS callback removal is outside the registry lock.

Each subscribed source gets a preallocated SPSC ring: 128 blocks of up to 256 frames,
eight float planes plus metadata. OBS serializes a source's capture callbacks.
Capture copies planes and scalar timestamp/gain/sync/epoch metadata, without
allocations, sleeps, network calls, file or UI operations. Full rings drop blocks
rather than blocking OBS. All conversion and timeline work runs on one worker.

The worker requests MMCSS Pro Audio priority, reports whether registration succeeds,
and restores it on exit. A high-resolution Windows waitable timer paces the worker; a separate stop event
interrupts its wait. Both destination sockets are nonblocking. Frontend EXIT
stops accepting capture, stops the worker, detaches callbacks, waits for pending
source destruction, releases taps and closes routing sockets. Module unload also
executes this idempotent shutdown.

## Timeline and output

Per-source capture data is converted by OBS's public resampler to stereo float at
the current OBS rate. Direct monotonic timestamps preserve source alignment;
foreign media clocks are anchored to monotonic arrival once, with bounded
discontinuity recovery. Small drift changes block length by at most one frame,
with interpolation across the preceding sample. Output positions remain contiguous;
uncorrected blocks preserve PCM exactly. Large jumps reanchor and are counted.

Each source has a bounded timestamp-addressed stereo timeline. Missing frames
become silence; duplicate or overlapping source frames are not summed twice.
Sync offsets are bounded to +/-20 seconds, covering the normal OBS UI range.
Timeline capacity expands on the worker to cover explicit positive offsets and
the shared look-behind needed for negative AV offsets. The base allowance is
60 ms by default (configurable from 20 to 200 ms). Negative AV sync extends shared latency so advancing audio does not cause
all its blocks to be discarded as late.

One 128-frame stereo block is summed for all participating sources. Fader gain is
applied once; the version-appropriate mute flag is applied once. Nonfinite and
out-of-range summed samples are sanitized/clamped at PCM24 conversion; there is
no compressor, limiter, auto-normalization or extra gain control.

PCM24 payload is encoded once. Each enabled destination gets its own stream-name
header, independent uint32 packet counter and socket. All sample-rate/header/name
constants come from the existing protocol definitions. UDP source ports are
ephemeral and independent of RX bind. An error sending to one destination does
not prevent sending to the other.

Enabled returns send continuous silence when no monitored audio contributes.
Status reports local sending, never an inferred remote connection.

### Sender adapter selection (0.2.1)

The optional root JSON key `return_local_ip` defaults to an empty string (Automatic).
The dropdown enumerates active/preferred IPv4 addresses using GetAdaptersAddresses,
only when the settings window opens; it does not rebuild an open dropdown.
Unavailable saved values remain visible. Explicit selection validates the address
against an active adapter, sets IP_UNICAST_IF using the adapter's network-order
index, and binds the UDP socket to that address with an ephemeral source port.
Both returns use the same selection, without touching the eight-input receiver.
Failure rejects the new configuration before persisting or replacing active sockets.

UDP connect establishes the destination association and selects a local route,
without a handshake or proof of remote reachability. getsockname supplies the actual
local endpoint for the UI and OBS log. The displayed address precedes router/NAT
translation. SIO_UDP_CONNRESET is disabled on return sockets so an offline UDP
destination does not poison later sends; sends remain independent and nonblocking.

Source/port/destination/stream and successful send age are reported. Stalled is
derived after one second without a successful send. Scheduling gaps over 20 ms
and the maximum observed gap are tracked on the worker, separately from socket errors.
Capture overflow events and late timeline frames are atomic per-source diagnostics;
aggregation is on status reads. These counters are local evidence, not network-loss
counters. Startup/monitor changes can add late frames, and removing a source removes
its lifetime counters. Waitable-timer failures now produce an error rather than
leaving a worker blocked behind a stale Sending indication.

References:
- https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-ip-socket-options
- https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-connect
- https://tailscale.com/docs/reference/troubleshooting/network-configuration/lan-traffic-overlapping-subnets

## Validation and practical limits

Automated validation includes the existing eight-input tests, core TX/ring/timeline
tests, real UDP fan-out, and actual plugin loading into both libobs 31.1.1 and the
official 32.2.1 runtime. The latter uses matching 32.2.1 frontend interface headers,
with a minimal test frontend and isolated configuration directory.

Tests exercise old configuration migration, initial/new monitored sources,
both monitoring modes, OFF, filters/gain, mute differences, 44.1 kHz mono input,
positive/negative AV sync, unbuffered decoupled AV, invalid Apply rollback,
independent destination enable, source activation/removal/replacement/churn,
and frontend EXIT while audio is active. Version 0.2.1 adds adapter-dropdown stability,
actual recvfrom source-address assertions for both returns, unavailable-adapter
rollback, endpoint/config persistence, stale sender state, and a two-second local
eight-source audio test checking packet continuity and PCM without gaps. Test input
timestamps follow the sample clock, independently of callback arrival jitter.

This is a reconstruction of source monitoring through public APIs. OBS's final
WASAPI device padding, async video presentation clock, device-specific suppression
and hardware effects are private. They cannot be reproduced bit-for-bit or used
to guarantee sample-identical timing with a physical headphone device. Hardware
self-monitor suppression is not used to exclude a source explicitly selected for
the VBAN return; this is a separate network destination.

Actual VoiceMeeter reception, listening, scene switching in the full OBS frontend,
and extended clock/load testing remain part of the user's target-PC test.
The running VoiceMeeter/Matrix routing on the development PC was not changed.

Version 0.2.2 adds `return_buffer_ms` to the existing JSON version 1. Omitted
values default to 60; invalid values are rejected before applying/saving.
Capture capacity is now 128 blocks per source. Status additionally reports
queue peak, clock corrections/discontinuities, clipped/nonfinite PCM samples,
and MMCSS availability. See [the audio review](AUDIO-REVIEW-0.2.2.md) for the
reproduced defect, timing/PCM regression tests and practical limits.
