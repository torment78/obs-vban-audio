// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vban-protocol.hpp"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
namespace vban {
// Single producer (OBS serializes a source's capture callbacks), single worker consumer.
template<class T, size_t Capacity> class AudioRing {
public:
    T *write_slot() {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) == Capacity) return nullptr;
        return &data_[w % Capacity];
    }
    void publish() { write_.fetch_add(1, std::memory_order_release); }
    const T *front() const {
        const auto r = read_.load(std::memory_order_relaxed);
        return r == write_.load(std::memory_order_acquire) ? nullptr : &data_[r % Capacity];
    }
    size_t size() const {
        const auto w = write_.load(std::memory_order_acquire);
        return static_cast<size_t>(std::min<uint64_t>(Capacity, w - read_.load(std::memory_order_acquire)));
    }
    void pop() { read_.fetch_add(1, std::memory_order_release); }
private:
    std::array<T, Capacity> data_{};
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};
constexpr size_t capture_frames = 256;
constexpr size_t capture_blocks = 128;
constexpr size_t return_frames = 128;
inline int64_t ns_to_frames(int64_t ns, uint32_t rate) {
    return (ns / 1000000000LL) * rate + (ns % 1000000000LL) * rate / 1000000000LL;
}
// OBS capture data is float-planar in the current OBS output rate/layout.
struct MonitorBlock {
    std::array<std::array<float, capture_frames>, 8> samples{};
    uint64_t timestamp = 0, arrival = 0, epoch = 0;
    int64_t sync_ns = 0;
    uint32_t rate = 0, frames = 0;
    uint8_t speakers = 0;
    float gain = 1.0f;
};
// Keep sample blocks contiguous while following independent source clocks.
// Corrections stretch/shrink by at most one frame per block; never skip/overwrite
// PCM at the old 1 ms rounding threshold. Large jumps still reanchor explicitly.
class MonitorClock {
public:
    int64_t position(uint64_t timestamp, uint64_t arrival, uint32_t frames, uint32_t rate) {
        const auto ts = ns_to_frames(static_cast<int64_t>(timestamp), rate);
        const auto now = ns_to_frames(static_cast<int64_t>(arrival), rate);
        adjusted_frames_ = frames;
        discontinuity_ = false;
        if (!initialized_ || std::llabs(ts - expected_timestamp_) > static_cast<int64_t>(rate / 2)) {
            discontinuity_ = initialized_;
            offset_ = std::llabs(ts - now) < static_cast<int64_t>(rate * 2) ? 0 : now - ts;
            next_ = ts + offset_;
            initialized_ = true;
            arrival_error_ = 0;
        }
        // Follow a foreign clock's real arrival rate through a one-second low-pass
        // estimate. The former 100 ms threshold exceeded the return buffer and
        // could keep long-running sources permanently late. Isolated callback jitter
        // must not move the anchor abruptly; correction remains one frame per block.
        if (offset_) {
            const double alpha = std::min(1.0, double(frames) / rate);
            arrival_error_ += (double(ts + offset_ - now) - arrival_error_) * alpha;
            if (std::fabs(arrival_error_) > double(rate) / 200.0) {
                const int correction = arrival_error_ < 0 ? 1 : -1;
                offset_ += correction; arrival_error_ += correction;
            }
        }
        const auto error = ts + offset_ - next_;
        if (std::llabs(error) > static_cast<int64_t>(rate / 100)) {
            next_ = ts + offset_; discontinuity_ = true;
        } else if (frames > 1 && std::llabs(error) > 1) {
            adjusted_frames_ = error > 0 ? frames + 1 : frames - 1;
        }
        const auto result = next_;
        next_ += adjusted_frames_;
        expected_timestamp_ = ts + frames;
        return result;
    }
    uint32_t frames() const { return adjusted_frames_; }
    bool discontinuity() const { return discontinuity_; }
    void reset() {
        initialized_ = discontinuity_ = false;
        offset_ = next_ = expected_timestamp_ = 0; adjusted_frames_ = 0; arrival_error_ = 0;
    }
private:
    bool initialized_ = false, discontinuity_ = false;
    int64_t offset_ = 0, next_ = 0, expected_timestamp_ = 0;
    uint32_t adjusted_frames_ = 0;
    double arrival_error_ = 0;
};
// Worker-only interpolation for tiny clock corrections. Preserve the previous
// sample across blocks, and pass PCM through exactly when no correction is needed.
class StereoRetimer {
public:
    const float *process(const float *input, size_t frames, size_t adjusted) {
        if (!frames || !adjusted) return input;
        if (!have_previous_) {
            previous_ = {input[0], input[1]}; have_previous_ = true;
        }
        const float *result = input;
        if (frames != adjusted) {
            output_.resize(adjusted * 2);
            for (size_t i = 0; i < adjusted; ++i) {
                const double position = double(i + 1) * double(frames) / double(adjusted) - 1.0;
                const auto a = static_cast<int64_t>(std::floor(position));
                const auto b = std::min<size_t>(static_cast<size_t>(a + 1), frames - 1);
                const float fraction = static_cast<float>(position - double(a));
                for (size_t c = 0; c < 2; ++c) {
                    const float left = a < 0 ? previous_[c] : input[static_cast<size_t>(a)*2+c];
                    output_[i*2+c] = left + (input[b*2+c] - left) * fraction;
                }
            }
            result = output_.data();
        }
        previous_ = {input[(frames-1)*2], input[(frames-1)*2+1]};
        return result;
    }
    void reset() { have_previous_ = false; }
private:
    bool have_previous_ = false;
    std::array<float, 2> previous_{};
    std::vector<float> output_;
};
// Worker-only timestamp-addressed stereo storage. Missing frames read as silence,
// repeated/overlapping frames are never summed twice for an individual source.
class StereoTimeline {
    struct Frame { int64_t tag = -1; float left = 0, right = 0; };
public:
    void resize(size_t frames) { if (frames != data_.size()) data_.assign(frames, Frame{}); }
    void clear() { for (auto &f : data_) f.tag = -1; }
    size_t put(int64_t start, const float *samples, size_t frames, float gain, int64_t cursor) {
        if (data_.empty()) return 0;
        size_t late = 0;
        for (size_t i = 0; i < frames; ++i) {
            const auto time = start + static_cast<int64_t>(i);
            if (time < cursor) { ++late; continue; }
            if (time >= cursor + static_cast<int64_t>(data_.size())) continue;
            auto &f = data_[static_cast<size_t>(time) % data_.size()];
            if (f.tag == time) continue;
            f = {time, samples[i*2] * gain, samples[i*2+1] * gain};
        }
        return late;
    }
    void mix(int64_t start, float *out, size_t frames) {
        if (data_.empty()) return;
        for (size_t i = 0; i < frames; ++i) {
            const auto time = start + static_cast<int64_t>(i);
            auto &f = data_[static_cast<size_t>(time) % data_.size()];
            if (f.tag != time) continue;
            out[i*2] += f.left; out[i*2+1] += f.right; f.tag = -1;
        }
    }
private:
    std::vector<Frame> data_;
};
}
