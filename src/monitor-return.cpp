// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <avrt.h>
#include "monitor-return.hpp"
#include "return-audio.hpp"
#include <obs.h>
#include <media-io/audio-resampler.h>
#include <util/platform.h>
#include <cstring>
#include <thread>
#include <stdexcept>
#include <unordered_map>

namespace vban {
struct MonitorReturn::Impl {
    struct Tap {
        Impl &owner;
        obs_weak_source_t *weak;
        signal_handler_t *signals;
        std::atomic<bool> monitored{false}, removed{false}, dead{false}, ready{false};
        std::atomic<uint64_t> epoch{1}, dropped{0}, late_frames{0}, clock_corrections{0}, discontinuities{0};
        std::atomic<size_t> queue_peak{0};
        std::atomic<int64_t> sync_hint{0};
        bool attached = false; // Worker only.
        uint64_t worker_epoch = 0;
        std::unique_ptr<AudioRing<MonitorBlock, capture_blocks>> ring;
        MonitorClock clock;
        StereoRetimer retimer;
        StereoTimeline timeline;
        audio_resampler_t *resampler = nullptr;
        uint32_t input_rate = 0, output_rate = 0;
        uint8_t speakers = 0;
        explicit Tap(Impl &engine, obs_source_t *source)
            : owner(engine), weak(obs_source_get_weak_source(source)),
              signals(obs_source_get_signal_handler(source)) {
            monitored = obs_source_get_monitoring_type(source) != OBS_MONITORING_TYPE_NONE;
            signal_handler_connect_ref(signals, "audio_monitoring", monitoring, this);
            signal_handler_connect_ref(signals, "deactivate", deactivate, this);
            signal_handler_connect_ref(signals, "remove", remove, this);
            signal_handler_connect_ref(signals, "destroy", destroy, this);
            ready = true;
        }
        ~Tap() {
            // Capture has been detached, or source destruction has already quiesced it.
            signal_handler_disconnect(signals, "audio_monitoring", monitoring, this);
            signal_handler_disconnect(signals, "deactivate", deactivate, this);
            signal_handler_disconnect(signals, "remove", remove, this);
            signal_handler_disconnect(signals, "destroy", destroy, this);
            obs_weak_source_release(weak);
            audio_resampler_destroy(resampler);
        }
        static void monitoring(void *p, calldata_t *data) {
            auto &tap = *static_cast<Tap *>(p);
            // This signal precedes OBS storing its new state: use the signal argument.
            const bool enabled = calldata_int(data, "type") != OBS_MONITORING_TYPE_NONE;
            if (tap.monitored.exchange(enabled) != enabled) ++tap.epoch;
        }
        static void deactivate(void *p, calldata_t *) { ++static_cast<Tap *>(p)->epoch; }
        static void remove(void *p, calldata_t *) {
            auto &tap = *static_cast<Tap *>(p); tap.removed = true; ++tap.epoch;
        }
        static void destroy(void *p, calldata_t *) {
            auto &tap = *static_cast<Tap *>(p); tap.dead = true; ++tap.epoch;
        }
        static void capture(void *p, obs_source_t *source, const audio_data *audio, bool muted) {
            auto &tap = *static_cast<Tap *>(p);
            if (tap.owner.stopping.load(std::memory_order_relaxed) || !tap.monitored.load() ||
                tap.removed.load() || !obs_source_active(source)) return;
            const auto *info = audio_output_get_info(obs_get_audio());
            if (!info || info->format != AUDIO_FORMAT_FLOAT_PLANAR) return;
            const auto channels = get_audio_channels(info->speakers);
            if (!channels || channels > 8) return;
            const auto arrival = os_gettime_ns();
            const auto generation = tap.epoch.load();
            const float gain = (!tap.owner.ignore_program_mute && muted) ? 0.0f : obs_source_get_volume(source);
            // Windows OBS applies source sync offsets only to sources with video.
            const bool decoupled = obs_source_async_unbuffered(source) && obs_source_async_decoupled(source);
            const int64_t sync = (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) && !decoupled
                ? obs_source_get_sync_offset(source) : 0;
            tap.sync_hint = std::clamp<int64_t>(sync, -20000000000LL, 20000000000LL);
            for (uint32_t start = 0; start < audio->frames; start += capture_frames) {
                auto *block = tap.ring->write_slot();
                if (!block) { ++tap.dropped; return; } // Bounded: never wait for the worker.
                block->frames = std::min<uint32_t>(capture_frames, audio->frames - start);
                block->timestamp = audio->timestamp + frames_to_ns(start, info->samples_per_sec);
                block->arrival = arrival + frames_to_ns(start, info->samples_per_sec);
                block->epoch = generation; block->rate = info->samples_per_sec;
                block->speakers = static_cast<uint8_t>(info->speakers);
                block->sync_ns = sync; block->gain = gain;
                for (uint32_t c = 0; c < channels; ++c) {
                    if (audio->data[c])
                        std::memcpy(block->samples[c].data(),
                            reinterpret_cast<const float *>(audio->data[c]) + start, block->frames * sizeof(float));
                    else std::fill_n(block->samples[c].data(), block->frames, 0.0f);
                }
                tap.ring->publish();
                const auto queued = tap.ring->size();
                if (queued > tap.queue_peak.load(std::memory_order_relaxed)) tap.queue_peak = queued;
            }
        }
        void detach() {
            if (!attached) return;
            auto *source = obs_weak_source_get_source(weak);
            if (source) {
                obs_source_remove_audio_capture_callback(source, capture, this);
                obs_source_release(source);
                attached = false;
            } else if (dead.load()) {
                // OBS clears its audio callback list before emitting source destroy.
                attached = false;
            }
        }
        void reset_audio() {
            timeline.clear(); clock.reset(); retimer.reset(); worker_epoch = epoch.load();
            audio_resampler_destroy(resampler); resampler = nullptr; input_rate = output_rate = 0;
        }
        void service(bool enabled, uint32_t rate, int64_t cursor, int64_t lookbehind_ns, float *mix) {
            if (!ready.load()) return;
            const bool wanted = enabled && monitored.load() && !removed.load() && !dead.load();
            if (!wanted) {
                detach();
                if (!attached) ring.reset();
                reset_audio(); return;
            }
            auto *source = obs_weak_source_get_source(weak);
            if (!source) return; // Keep Tap alive until the destruction signal quiesces callbacks.
            if (!attached) {
                ring = std::make_unique<AudioRing<MonitorBlock, capture_blocks>>();
                ++epoch; reset_audio();
                obs_source_add_audio_capture_callback(source, capture, this);
                attached = true;
            }
            const bool active = obs_source_active(source);
            obs_source_release(source);
            if (worker_epoch != epoch.load()) reset_audio();
            if (output_rate && output_rate != rate) reset_audio();
            size_t processed = 0;
            while (const auto *block = ring->front()) {
                if (++processed > capture_blocks) break; // A busy producer cannot starve other sources.
                if (block->epoch != worker_epoch || !active) { ring->pop(); continue; }
                if (!resampler || input_rate != block->rate || output_rate != rate || speakers != block->speakers) {
                    audio_resampler_destroy(resampler);
                    const resample_info from{block->rate, AUDIO_FORMAT_FLOAT_PLANAR, static_cast<speaker_layout>(block->speakers)};
                    const resample_info to{rate, AUDIO_FORMAT_FLOAT, SPEAKERS_STEREO};
                    resampler = audio_resampler_create(&to, &from);
                    input_rate = block->rate; output_rate = rate; speakers = block->speakers;
                    timeline.clear(); clock.reset(); retimer.reset();
                }
                if (resampler) {
                    std::array<const uint8_t *, MAX_AV_PLANES> input{};
                    for (size_t c = 0; c < get_audio_channels(static_cast<speaker_layout>(block->speakers)); ++c)
                        input[c] = reinterpret_cast<const uint8_t *>(block->samples[c].data());
                    uint8_t *output[MAX_AV_PLANES]{};
                    uint32_t frames = 0; uint64_t offset = 0;
                    if (audio_resampler_resample(resampler, output, &frames, &offset, input.data(), block->frames) && frames && output[0]) {
                        // Bound deliberately extreme offsets; OBS's normal UI range fits within +/-20 s.
                        const auto delay = std::clamp<int64_t>(block->sync_ns, -20000000000LL, 20000000000LL);
                        const auto capacity_seconds = 1 + static_cast<size_t>((std::max<int64_t>(0, delay) + lookbehind_ns + 999999999) / 1000000000);
                        timeline.resize(static_cast<size_t>(rate) * capacity_seconds);
                        const auto position = clock.position(block->timestamp, block->arrival, frames, rate)
                            + ns_to_frames(delay - static_cast<int64_t>(offset), rate);
                        if (clock.discontinuity()) { ++discontinuities; retimer.reset(); timeline.clear(); }
                        if (clock.frames() != frames) ++clock_corrections;
                        const auto *pcm = retimer.process(reinterpret_cast<const float *>(output[0]), frames, clock.frames());
                        late_frames += timeline.put(position, pcm, clock.frames(), block->gain, cursor);
                    }
                }
                ring->pop();
            }
            if (active && monitored.load() && !removed.load() && worker_epoch == epoch.load())
                timeline.mix(cursor, mix, return_frames);
        }
    };
    Transmitter tx;
    std::atomic<bool> stopping{false};
    bool started = false;
    std::atomic<uint32_t> buffer_ms{default_return_buffer_ms};
    std::atomic<bool> mmcss{false};
    const bool ignore_program_mute = obs_get_version() >= ((32u << 24) | (2u << 16));
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    std::thread worker;
    std::mutex registry_mutex;
    std::unordered_map<obs_source_t *, std::shared_ptr<Tap>> taps;
    Impl() {
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!stop_event || !timer) {
            if (stop_event) CloseHandle(stop_event);
            if (timer) CloseHandle(timer);
            throw std::runtime_error("Cannot create monitor return worker timer.");
        }
    }
    ~Impl() { shutdown(); CloseHandle(timer); CloseHandle(stop_event); }
    void track(obs_source_t *source) {
        if (stopping || obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT ||
            !(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) return;
        // Registry operations never run in the source audio callback.
        std::shared_ptr<Tap> retired;
        {
            std::lock_guard lock(registry_mutex);
            const auto it = taps.find(source);
            if (it != taps.end()) {
                if (!it->second->dead.load()) return;
                retired = std::move(it->second); taps.erase(it);
            }
        }
        retired.reset(); // OBS may reuse a destroyed source address before the next worker tick.
        auto tap = std::make_shared<Tap>(*this, source);
        std::lock_guard lock(registry_mutex);
        taps.emplace(source, std::move(tap));
    }
    static void created(void *p, calldata_t *data) {
        try { static_cast<Impl *>(p)->track(static_cast<obs_source_t *>(calldata_ptr(data, "source"))); }
        catch (const std::exception &e) { static_cast<Impl *>(p)->tx.fail(e.what()); }
    }
    static bool enumerate(void *p, obs_source_t *source) {
        static_cast<Impl *>(p)->track(source); return true;
    }
    void start() {
        if (started) return;
        started = true;
        signal_handler_connect(obs_get_signal_handler(), "source_create", created, this);
        obs_enum_sources(enumerate, this);
        worker = std::thread([this] {
            try { run(); }
            catch (const std::exception &e) { tx.fail(e.what()); stopping = true; }
        });
    }
    void run() {
        // MMCSS prioritizes just this audio worker; no system-wide timer/priority changes.
        struct Scheduling {
            DWORD index = 0;
            HANDLE handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
            ~Scheduling() { if (handle) AvRevertMmThreadCharacteristics(handle); }
        } scheduling;
        mmcss = scheduling.handle != nullptr;
        std::vector<std::shared_ptr<Tap>> live;
        uint32_t rate = 0;
        int64_t cursor = 0, previous_latency_ns = 0;
        while (!stopping.load()) {
            const auto now = os_gettime_ns();
            const auto *info = audio_output_get_info(obs_get_audio());
            const uint32_t current_rate = info ? info->samples_per_sec : 48000;
            const auto current = ns_to_frames(static_cast<int64_t>(now), current_rate);
            const bool enabled = tx.enabled();
            live.clear();
            {
                std::lock_guard lock(registry_mutex);
                for (auto it = taps.begin(); it != taps.end();) {
                    if (it->second->dead.load()) it = taps.erase(it);
                    else { live.push_back(it->second); ++it; }
                }
            }
            // Negative AV sync needs look-behind: otherwise advancing already-arrived
            // audio would drop every block as late. One shared latency preserves relative alignment.
            int64_t lookbehind_ns = 0;
            for (const auto &tap : live)
                if (tap->monitored.load() && !tap->removed.load())
                    lookbehind_ns = std::max(lookbehind_ns, -tap->sync_hint.load());
            const int64_t latency_ns = int64_t(buffer_ms.load()) * 1000000 + lookbehind_ns;
            const auto latency = ns_to_frames(latency_ns, current_rate);
            if (rate != current_rate || previous_latency_ns != latency_ns || !cursor ||
                current - cursor > latency + static_cast<int64_t>(current_rate)) {
                cursor = current - latency;
                for (const auto &tap : live) tap->reset_audio();
            }
            rate = current_rate; previous_latency_ns = latency_ns;
            const auto target = current - latency;
            // Catch up only a bounded number of packets after scheduling delays.
            size_t count = 0;
            while (cursor + static_cast<int64_t>(return_frames) <= target && count++ < 32) {
                std::array<float, return_frames * 2> mix{};
                for (const auto &tap : live) tap->service(enabled, rate, cursor, lookbehind_ns, mix.data());
                if (enabled) tx.send(mix.data(), return_frames, rate);
                cursor += return_frames;
            }
            LARGE_INTEGER due{};
            const auto remaining = std::max<int64_t>(100000, (cursor + static_cast<int64_t>(return_frames) - target) * 1000000000LL / rate);
            due.QuadPart = -(remaining / 100);
            if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                throw std::runtime_error("Monitor return scheduling timer failed (Windows " + std::to_string(GetLastError()) + ").");
            const HANDLE handles[]{stop_event, timer};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_FAILED)
                throw std::runtime_error("Monitor return worker wait failed (Windows " + std::to_string(GetLastError()) + ").");
        }
    }
    void shutdown() {
        if (!started) return;
        stopping = true;
        signal_handler_disconnect(obs_get_signal_handler(), "source_create", created, this);
        SetEvent(stop_event);
        if (worker.joinable()) worker.join();
        for (auto &entry : taps) entry.second->detach();
        // A failed weak upgrade can mean destruction has started but not yet emitted destroy.
        obs_wait_for_destroy_queue();
        for (auto &entry : taps) entry.second->detach();
        taps.clear();
        tx.activate({});
        started = false;
    }
};
MonitorReturn::MonitorReturn() : impl_(std::make_unique<Impl>()) {}
MonitorReturn::~MonitorReturn() = default;
void MonitorReturn::start() { impl_->start(); }
void MonitorReturn::shutdown() { impl_->shutdown(); }
Transmitter::Prepared MonitorReturn::prepare(const ReturnConfigs &cfg, std::string &error, const std::string &local_ip, uint32_t buffer_ms) {
    if (buffer_ms < min_return_buffer_ms || buffer_ms > max_return_buffer_ms) {
        error = "Return audio buffer must be 20 to 200 ms."; return {};
    }
    if (impl_->stopping.load()) { error = "Monitor return worker stopped. Restart OBS."; return {}; }
    return impl_->tx.prepare(cfg, error, local_ip);
}
void MonitorReturn::activate(Transmitter::Prepared cfg, uint32_t buffer_ms) {
    impl_->buffer_ms = std::clamp(buffer_ms, min_return_buffer_ms, max_return_buffer_ms);
    impl_->tx.activate(std::move(cfg));
}
ReturnStatus MonitorReturn::status(size_t i) const {
    auto result = impl_->tx.status(i);
    result.buffer_ms = impl_->buffer_ms.load();
    result.audio_priority = impl_->mmcss.load();
    result.capture_queue_capacity = capture_blocks;
    std::lock_guard lock(impl_->registry_mutex);
    for (const auto &entry : impl_->taps) {
        result.capture_drops += entry.second->dropped.load();
        result.late_audio_frames += entry.second->late_frames.load();
        result.clock_corrections += entry.second->clock_corrections.load();
        result.clock_discontinuities += entry.second->discontinuities.load();
        result.capture_queue_peak = std::max(result.capture_queue_peak, entry.second->queue_peak.load());
    }
    return result;
}
}
