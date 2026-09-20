// SPDX-License-Identifier: GPL-2.0-or-later
#include <winsock2.h>
#include <ws2tcpip.h>
#include "receiver.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

namespace vban {
struct Receiver::Socket {
    SOCKET handle = INVALID_SOCKET;
    ~Socket() { if (handle != INVALID_SOCKET) closesocket(handle); }
};
static bool ipv4(const std::string &text, uint32_t &address) {
    IN_ADDR parsed{};
    if (InetPtonA(AF_INET, text.c_str(), &parsed) != 1) return false;
    address = parsed.S_un.S_addr;
    return true;
}
std::string validate(const Config &cfg) {
    if (!cfg.port) return "UDP port must be 1 to 65535.";
    std::set<std::pair<uint32_t, std::string>> definitions;
    for (size_t i = 0; i < slot_count; ++i) {
        if (!cfg.slots[i].enabled) continue;
        const std::string prefix = "Slot " + std::to_string(i+1) + ": ";
        uint32_t address = 0;
        if (!ipv4(cfg.ip(i), address)) return prefix + "Enter a valid sender IPv4 address.";
        if (!valid_stream_name(cfg.slots[i].stream_name))
            return prefix + "Stream name must contain 1 to 16 printable ASCII characters.";
        if (!definitions.emplace(address, cfg.slots[i].stream_name).second)
            return prefix + "This sender and stream are already configured. Select the same slot in multiple OBS sources instead.";
    }
    return {};
}
const char *state_name(State state) {
    switch (state) {
    case State::disabled: return "Disabled";
    case State::waiting: return "Waiting";
    case State::receiving: return "Receiving";
    case State::error: return "Error";
    }
    return "Error";
}
Receiver::Receiver(std::function<uint64_t()> clock, Logger logger)
    : routing_(std::make_shared<Routing>()), clock_(std::move(clock)), logger_(std::move(logger)) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data)) throw std::runtime_error("Winsock initialization failed");
    try {
        network_ = std::thread([this] { try { receive_loop(); } catch (const std::exception &e) { fail(e.what()); } });
        audio_ = std::thread([this] { try { audio_loop(); } catch (const std::exception &e) { fail(e.what()); } });
    } catch (...) {
        stop_ = true; wake_.notify_all();
        if (network_.joinable()) network_.join();
        WSACleanup();
        throw;
    }
}
Receiver::~Receiver() {
    shutdown();
    routing_.reset();
    WSACleanup();
}
void Receiver::shutdown() {
    stop_ = true; wake_.notify_all();
    if (network_.joinable()) network_.join();
    if (audio_.joinable()) audio_.join();
}
void Receiver::log(bool warning, const std::string &message) const {
    if (logger_) logger_(warning, message);
}
void Receiver::fail(const std::string &error) {
    { std::lock_guard lock(config_mutex_); fatal_error_ = error; }
    stop_ = true; wake_.notify_all();
    log(true, error);
}
Config Receiver::config() const {
    std::lock_guard lock(config_mutex_);
    return routing_->config;
}
bool Receiver::configure(const Config &cfg, std::string &error, const Save &save) {
    std::lock_guard apply(apply_mutex_);
    error = validate(cfg);
    if (!error.empty()) return false;
    std::shared_ptr<Routing> old;
    { std::lock_guard lock(config_mutex_); old = routing_; }
    if (stop_) { error = "Receiver worker stopped. Restart OBS."; return false; }
    auto next = std::make_shared<Routing>();
    next->config = cfg;
    for (size_t i = 0; i < slot_count; ++i)
        if (cfg.slots[i].enabled) ipv4(cfg.ip(i), next->addresses[i]);
    const bool enabled = std::any_of(cfg.slots.begin(), cfg.slots.end(), [](const auto &s) { return s.enabled; });
    if (enabled && old->socket && old->config.port == cfg.port) next->socket = old->socket;
    else if (enabled) {
        auto socket = std::make_shared<Socket>();
        socket->handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL exclusive = TRUE;
        int receive_size = 1024 * 1024;
        if (socket->handle == INVALID_SOCKET ||
            setsockopt(socket->handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
            error = "Cannot create UDP socket (Winsock " + std::to_string(WSAGetLastError()) + ").";
            return false;
        }
        setsockopt(socket->handle, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char *>(&receive_size), sizeof(receive_size));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(cfg.port);
        if (bind(socket->handle, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
            error = "Cannot bind UDP port " + std::to_string(cfg.port) + " (Winsock " +
                std::to_string(WSAGetLastError()) + "). Another receiver may already use this port.";
            log(true, error);
            return false;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(socket->handle, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            error = "Could not set UDP socket to nonblocking mode.";
            return false;
        }
        next->socket = std::move(socket);
    }
    // Validate/bind before saving; a rejected change leaves the active configuration intact.
    if (save && !save(cfg, error)) return false;
    {
        std::lock_guard config_lock(config_mutex_);
        next->generation = generation_.fetch_add(1) + 1;
        for (size_t i = 0; i < slot_count; ++i) {
            const auto &a = old->config.slots[i], &b = cfg.slots[i];
            if (a.enabled != b.enabled || a.stream_name != b.stream_name ||
                old->config.ip(i) != cfg.ip(i) || old->config.port != cfg.port) {
                std::lock_guard slot_lock(slots_[i].mutex);
                slots_[i].buffer.reset();
                slots_[i].error.clear();
                slots_[i].was_receiving = false;
            }
        }
        routing_ = std::move(next);
        fatal_error_.clear();
    }
    wake_.notify_all();
    log(false, enabled ? "Listening on UDP " + std::to_string(cfg.port) +
        ". If waiting, check sender IP, stream name, port, Windows Firewall, and the sender's VBAN output."
        : "All receive slots are disabled.");
    return true;
}
Status Receiver::status(size_t index) const {
    if (index >= slot_count) return {};
    std::lock_guard config_lock(config_mutex_);
    const auto &slot = slots_[index];
    std::lock_guard slot_lock(slot.mutex);
    Status result;
    result.format = slot.buffer.format();
    result.counters = slot.buffer.counters();
    if (!routing_->config.slots[index].enabled) return result;
    result.error = fatal_error_.empty() ? slot.error : fatal_error_;
    if (!result.error.empty()) result.state = State::error;
    else if (slot.buffer.last_valid() && clock_() - slot.buffer.last_valid() < 1000000000ULL)
        result.state = State::receiving;
    else result.state = State::waiting;
    return result;
}
std::shared_ptr<Receiver::Consumer> Receiver::subscribe(int slot, std::function<void(const AudioBlock &)> output) {
    auto consumer = std::make_shared<Consumer>();
    consumer->slot = slot;
    consumer->output = std::move(output);
    std::lock_guard lock(consumers_mutex_);
    consumers_.push_back(consumer);
    return consumer;
}
void Receiver::unsubscribe(const std::shared_ptr<Consumer> &consumer) {
    consumer->slot = -1;
    std::lock_guard lock(consumers_mutex_);
    consumers_.erase(std::remove(consumers_.begin(), consumers_.end(), consumer), consumers_.end());
}
void Receiver::receive_loop() {
    std::array<uint8_t, 65536> data{};
    while (!stop_) {
        std::shared_ptr<Routing> route;
        { std::lock_guard lock(config_mutex_); route = routing_; }
        if (!route->socket) {
            std::unique_lock lock(wait_mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
        fd_set readable; FD_ZERO(&readable); FD_SET(route->socket->handle, &readable);
        timeval timeout{0, 20000};
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR) { fail("UDP select failed: " + std::to_string(WSAGetLastError())); return; }
        if (!ready) continue;
        sockaddr_in sender{};
        int length = sizeof(sender);
        const int count = recvfrom(route->socket->handle, reinterpret_cast<char *>(data.data()),
            static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr *>(&sender), &length);
        if (count == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            if (code == WSAEWOULDBLOCK || code == WSAEMSGSIZE || code == WSAECONNRESET) continue;
            fail("UDP receive failed: " + std::to_string(code)); return;
        }
        {
            std::lock_guard lock(config_mutex_);
            if (route->socket != routing_->socket) continue;
            route = routing_; // A live edit may have occurred while select was waiting.
        }
        std::string name;
        if (!read_stream_name(data.data(), static_cast<size_t>(count), name)) continue;
        for (size_t i = 0; i < slot_count; ++i) {
            if (!route->config.slots[i].enabled || sender.sin_addr.s_addr != route->addresses[i] ||
                name != route->config.slots[i].stream_name) continue;
            Packet packet;
            const auto error = decode(data.data(), static_cast<size_t>(count), packet);
            const uint64_t now = clock_();
            std::string message;
            {
                auto &slot = slots_[i];
                std::lock_guard lock(slot.mutex);
                if (route->generation != generation_.load()) break;
                if (error != ParseError::none) {
                    const bool unsupported = error == ParseError::codec || error == ParseError::protocol ||
                        error == ParseError::sample_type || error == ParseError::channels;
                    if (unsupported) ++slot.buffer.counters().unsupported;
                    else ++slot.buffer.counters().corrupt;
                    slot.error = describe(error);
                    if (now >= slot.warning_time) {
                        message = "Slot " + std::to_string(i+1) + ": " + slot.error;
                        slot.warning_time = now + 5000000000ULL;
                    }
                } else {
                    if (!slot.was_receiving || slot.buffer.format() != packet.format) {
                        message = "Slot " + std::to_string(i+1) + ": receiving " + name + " at " +
                            std::to_string(packet.format.rate) + " Hz, " +
                            std::to_string(packet.format.channels) + " ch, " + format_name(packet.format.type);
                    }
                    slot.was_receiving = true;
                    slot.error.clear();
                    slot.buffer.push(std::move(packet), now);
                }
            }
            if (!message.empty()) log(error != ParseError::none, message);
        }
    }
}
void Receiver::audio_loop() {
    while (!stop_) {
        const auto now = clock_();
        const uint64_t generation = generation_.load();
        for (size_t i = 0; i < slot_count; ++i) {
            std::optional<AudioBlock> block;
            bool timed_out = false;
            {
                std::lock_guard lock(slots_[i].mutex);
                block = slots_[i].buffer.pull(now);
                if (slots_[i].was_receiving && now - slots_[i].buffer.last_valid() >= 1000000000ULL) {
                    slots_[i].was_receiving = false; timed_out = true;
                }
            }
            if (timed_out) log(false, "Slot " + std::to_string(i+1) + ": waiting for packets.");
            if (!block || generation != generation_.load()) continue;
            std::vector<std::shared_ptr<Consumer>> consumers;
            { std::lock_guard lock(consumers_mutex_); consumers = consumers_; }
            for (const auto &consumer : consumers) {
                if (consumer->slot.load() == static_cast<int>(i) && consumer->output) consumer->output(*block);
            }
        }
        std::unique_lock lock(wait_mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(2));
    }
}
}
