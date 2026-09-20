// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vban-protocol.hpp"
#include <deque>
#include <optional>
#include <unordered_map>

namespace vban {
struct Counters {
    uint64_t received = 0, lost = 0, duplicates = 0, reordered = 0, late = 0;
    uint64_t corrupt = 0, unsupported = 0, underruns = 0, overruns = 0;
    uint64_t discontinuities = 0, drift_corrections = 0;
};
struct AudioBlock {
    Format format;
    uint32_t frames = 0;
    uint64_t timestamp = 0;
    std::vector<float> samples;
};
// Access is serialized by each receiver slot's mutex. No OBS or Qt dependency.
class StreamBuffer {
public:
    explicit StreamBuffer(uint32_t target_ms = 30) : target_ms_(target_ms) {}
    void push(Packet packet, uint64_t now);
    std::optional<AudioBlock> pull(uint64_t now);
    void reset(); // Keeps the last output timestamp across reconfiguration.
    const Counters &counters() const { return counters_; }
    Counters &counters() { return counters_; }
    const Format &format() const { return format_; }
    uint64_t last_valid() const { return last_valid_; }
private:
    void begin(const Packet &packet, uint64_t now);
    size_t buffered_frames() const;
    bool fill(size_t frames);
    uint32_t target_ms_;
    Format format_;
    Counters counters_;
    std::unordered_map<uint32_t, Packet> pending_;
    std::deque<float> fifo_;
    std::deque<uint32_t> recent_;
    uint32_t expected_ = 0, newest_ = 0;
    uint16_t previous_frames_ = 0;
    uint64_t first_ = 0, last_valid_ = 0, last_progress_ = 0;
    uint64_t base_ = 0, emitted_ = 0, last_end_ = 0;
    bool initialized_ = false, playing_ = false;
};
}
