// SPDX-License-Identifier: GPL-2.0-or-later
#include "stream-buffer.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace vban {
void StreamBuffer::reset() {
    pending_.clear(); fifo_.clear(); recent_.clear();
    format_ = {}; counters_ = {};
    first_ = last_valid_ = last_progress_ = base_ = emitted_ = 0;
    previous_frames_ = 0;
    initialized_ = playing_ = false;
}
void StreamBuffer::begin(const Packet &packet, uint64_t now) {
    pending_.clear(); fifo_.clear(); recent_.clear();
    format_ = packet.format;
    expected_ = newest_ = packet.sequence;
    first_ = last_progress_ = now;
    playing_ = false;
    initialized_ = true;
    previous_frames_ = 0;
}
size_t StreamBuffer::buffered_frames() const {
    size_t n = format_.channels ? fifo_.size() / format_.output_channels() : 0;
    for (const auto &item : pending_) n += item.second.frames;
    return n;
}
void StreamBuffer::push(Packet packet, uint64_t now) {
    ++counters_.received;
    last_valid_ = now;
    if (!initialized_ || packet.format != format_ || now - last_progress_ > 250000000ULL) {
        if (initialized_) ++counters_.discontinuities;
        begin(packet, now);
    }
    if (pending_.count(packet.sequence) ||
        std::find(recent_.begin(), recent_.end(), packet.sequence) != recent_.end()) {
        ++counters_.duplicates;
        return;
    }
    const uint32_t distance = packet.sequence - expected_;
    if (distance >= 0x80000000U) {
        ++counters_.late;
        return;
    }
    if (distance > 512 || pending_.size() >= 512 || buffered_frames() > format_.rate / 4) {
        ++counters_.discontinuities;
        ++counters_.overruns;
        begin(packet, now);
    }
    const uint32_t from_newest = packet.sequence - newest_;
    if (from_newest >= 0x80000000U) ++counters_.reordered;
    else newest_ = packet.sequence;
    last_progress_ = now;
    pending_.emplace(packet.sequence, std::move(packet));
}
bool StreamBuffer::fill(size_t frames) {
    const auto channels = format_.output_channels();
    while (fifo_.size() / channels < frames) {
        auto it = pending_.find(expected_);
        if (it == pending_.end()) {
            if (pending_.empty()) return false;
            auto next = pending_.end();
            uint32_t gap = UINT32_MAX;
            for (auto candidate = pending_.begin(); candidate != pending_.end(); ++candidate) {
                uint32_t d = candidate->first - expected_;
                if (d < gap) { gap = d; next = candidate; }
            }
            if (next == pending_.end()) return false;
            // Estimate missing audio only between packets of the same size and for short gaps.
            if (gap <= 8 && previous_frames_ && previous_frames_ == next->second.frames) {
                fifo_.insert(fifo_.end(), size_t(previous_frames_) * channels, 0.0f);
                ++counters_.lost;
                ++expected_;
                continue;
            }
            counters_.lost += gap;
            ++counters_.discontinuities;
            expected_ = next->first;
            it = next;
        }
        fifo_.insert(fifo_.end(), it->second.samples.begin(), it->second.samples.end());
        previous_frames_ = it->second.frames;
        recent_.push_back(expected_);
        if (recent_.size() > 128) recent_.pop_front();
        pending_.erase(it);
        ++expected_;
    }
    return true;
}
std::optional<AudioBlock> StreamBuffer::pull(uint64_t now) {
    if (!initialized_ || !format_.rate) return {};
    if (now - last_progress_ > 250000000ULL) {
        pending_.clear(); fifo_.clear(); initialized_ = false;
        playing_ = false;
        return {};
    }
    if (!playing_) {
        if (now < first_ + uint64_t(target_ms_) * 1000000ULL) return {};
        base_ = std::max(now, last_end_);
        emitted_ = 0;
        playing_ = true;
    }
    uint64_t timestamp = base_ + frames_to_ns(emitted_, format_.rate);
    if (now < timestamp) return {};
    if (now - timestamp > 100000000ULL) {
        // A suspended host must not replay a backlog with stale timestamps.
        ++counters_.discontinuities;
        pending_.clear(); fifo_.clear();
        initialized_ = playing_ = false;
        return {};
    }
    const uint32_t frames = std::max(1U, format_.rate / 100); // About 10 ms.
    const size_t queued = buffered_frames();
    const size_t target = size_t(format_.rate) * target_ms_ / 1000;
    const size_t margin = std::max<size_t>(frames, target / 3);
    size_t consume = frames;
    // Gentle queue-depth correction follows independent sender clocks without timestamp jumps.
    if (queued > target + margin) { ++consume; ++counters_.drift_corrections; }
    else if (queued + margin < target && queued >= frames && frames > 1) {
        --consume; ++counters_.drift_corrections;
    }
    const bool underflow = !fill(consume);
    if (underflow) ++counters_.underruns;
    const size_t channels = format_.output_channels();
    std::vector<float> input(consume * channels, 0.0f);
    const size_t available = std::min(input.size(), fifo_.size());
    for (size_t i = 0; i < available; ++i) { input[i] = fifo_.front(); fifo_.pop_front(); }
    AudioBlock block{format_, frames, timestamp, std::vector<float>(size_t(frames) * channels)};
    if (consume == frames) block.samples = std::move(input);
    else {
        for (uint32_t f = 0; f < frames; ++f) {
            const double pos = frames > 1 ? double(f) * double(consume-1) / double(frames-1) : 0;
            const size_t a = static_cast<size_t>(pos), b = std::min(a+1, consume-1);
            const float mix = static_cast<float>(pos - double(a));
            for (size_t c = 0; c < channels; ++c)
                block.samples[size_t(f)*channels+c] = input[a*channels+c] * (1-mix) + input[b*channels+c] * mix;
        }
    }
    emitted_ += frames;
    last_end_ = base_ + frames_to_ns(emitted_, format_.rate);
    if (underflow) { initialized_ = playing_ = false; pending_.clear(); fifo_.clear(); }
    return block;
}
}
