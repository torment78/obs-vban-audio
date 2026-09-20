// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <string>
namespace vban {
constexpr size_t return_count = 2;
constexpr uint32_t default_return_buffer_ms = 60;
constexpr uint32_t min_return_buffer_ms = 20, max_return_buffer_ms = 200;
struct ReturnConfig {
    bool enabled = false;
    std::string destination_ip;
    uint16_t destination_port = 6980;
    std::string stream_name;
};
using ReturnConfigs = std::array<ReturnConfig, return_count>;
inline ReturnConfigs default_returns() {
    return {{{false, "", 6980, "OBSRETURN1"}, {false, "", 6980, "OBSRETURN2"}}};
}
}
