// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "stream-buffer.hpp"
#include "return-config.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace vban {
struct SlotConfig {
    bool enabled = false;
    std::string label, sender_ip, stream_name;
};
struct Config {
    bool common_ip = true;
    std::string sender_ip;
    uint16_t port = 6980;
    std::array<SlotConfig, slot_count> slots;
    ReturnConfigs returns = default_returns();
    uint32_t return_buffer_ms = default_return_buffer_ms;
    std::string return_local_ip; // Empty: let Windows select the sending adapter.
    std::string ip(size_t i) const { return common_ip ? sender_ip : slots[i].sender_ip; }
};
std::string validate(const Config &config);
enum class State { disabled, waiting, receiving, error };
struct Status {
    State state = State::disabled;
    Format format;
    Counters counters;
    std::string error;
};
const char *state_name(State state);

class Receiver {
public:
    using Logger = std::function<void(bool warning, const std::string &)>;
    using Save = std::function<bool(const Config &, std::string &)>;
    struct Consumer {
        std::atomic<int> slot{-1};
        std::function<void(const AudioBlock &)> output;
    };
    explicit Receiver(std::function<uint64_t()> clock, Logger logger = {});
    ~Receiver();
    void shutdown();
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;
    bool configure(const Config &, std::string &error, const Save &save = {});
    Config config() const;
    Status status(size_t index) const;
    std::shared_ptr<Consumer> subscribe(int slot, std::function<void(const AudioBlock &)> output);
    void unsubscribe(const std::shared_ptr<Consumer> &consumer);
private:
    struct Socket;
    struct Slot {
        mutable std::mutex mutex;
        StreamBuffer buffer;
        std::string error;
        uint64_t warning_time = 0;
        bool was_receiving = false;
    };
    struct Routing {
        Config config;
        std::array<uint32_t, slot_count> addresses{};
        std::shared_ptr<Socket> socket;
        uint64_t generation = 0;
    };
    void receive_loop();
    void audio_loop();
    void fail(const std::string &error);
    void log(bool warning, const std::string &message) const;
    mutable std::mutex config_mutex_;
    std::mutex apply_mutex_;
    std::shared_ptr<Routing> routing_;
    std::string fatal_error_;
    std::array<Slot, slot_count> slots_;
    std::mutex consumers_mutex_;
    std::vector<std::shared_ptr<Consumer>> consumers_;
    std::function<uint64_t()> clock_;
    Logger logger_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> generation_{0};
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    std::thread network_, audio_;
};
}
