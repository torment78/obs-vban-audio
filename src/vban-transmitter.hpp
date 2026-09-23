// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "return-config.hpp"
#include "vban-protocol.hpp"
#include <memory>
#include <mutex>
#include <chrono>
#include <vector>
namespace vban {
struct LocalIPv4 {
    std::string address, adapter_name;
    uint32_t interface_index = 0;
};
std::vector<LocalIPv4> local_ipv4_addresses(std::string &error);
enum class ReturnState { disabled, ready, sending, stalled, error };
struct ReturnStatus {
    ReturnState state = ReturnState::disabled;
    uint64_t packets = 0, errors = 0;
    bool silence = true;
    std::string detail, source_ip, destination_ip, stream_name;
    uint16_t source_port = 0, destination_port = 0;
    uint64_t capture_drops = 0, late_audio_frames = 0, send_gaps = 0;
    uint64_t clipped_samples = 0, nonfinite_samples = 0, clock_corrections = 0, clock_discontinuities = 0;
    size_t capture_queue_peak = 0, capture_queue_capacity = 0;
    int pcm_bits = 24;
    uint32_t buffer_ms = default_return_buffer_ms;
    bool audio_priority = false;
    double max_send_gap_ms = 0, last_send_age_ms = 0;
};
const char *return_state_name(ReturnState state);
size_t encode_stereo_pcm(uint8_t *out, uint32_t rate, const std::string &name,
                         uint32_t sequence, const float *samples, size_t frames, int pcm_bits);
class Transmitter {
public:
    struct Routing;
    using Prepared = std::shared_ptr<Routing>;
    Transmitter();
    ~Transmitter();
    Prepared prepare(const ReturnConfigs &, std::string &error, const std::string &local_ip = {});
    void activate(Prepared routing);
    bool enabled() const;
    void send(const float *stereo, size_t frames, uint32_t rate);
    void fail(const std::string &error);
    ReturnStatus status(size_t i) const;
private:
    mutable std::mutex mutex_;
    Prepared routing_;
    std::array<uint32_t, return_count> sequences_{};
    std::array<ReturnStatus, return_count> status_{};
    std::array<std::chrono::steady_clock::time_point, return_count> last_send_{};
};
}
