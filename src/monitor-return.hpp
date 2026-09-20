// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vban-transmitter.hpp"
namespace vban {
class MonitorReturn {
public:
    MonitorReturn();
    ~MonitorReturn();
    void start();
    void shutdown();
    Transmitter::Prepared prepare(const ReturnConfigs &cfg, std::string &error, const std::string &local_ip = {},
                                  uint32_t buffer_ms = default_return_buffer_ms);
    void activate(Transmitter::Prepared config, uint32_t buffer_ms = default_return_buffer_ms);
    ReturnStatus status(size_t i) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
