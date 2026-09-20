// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vban {
constexpr size_t slot_count = 8;
constexpr size_t header_size = 28;
constexpr size_t max_datagram = 1464;
constexpr std::array<uint32_t, 21> sample_rates{
    6000,12000,24000,48000,96000,192000,384000,
    8000,16000,32000,64000,128000,256000,512000,
    11025,22050,44100,88200,176400,352800,705600};

struct Format {
    uint32_t rate = 0;
    uint8_t channels = 0;
    uint8_t type = 0;
    uint8_t output_channels() const { return channels == 7 ? 8 : channels; }
    bool operator==(const Format &o) const {
        return rate == o.rate && channels == o.channels && type == o.type;
    }
    bool operator!=(const Format &o) const { return !(*this == o); }
};
struct Packet {
    Format format;
    uint32_t sequence = 0;
    uint16_t frames = 0;
    std::string name;
    std::vector<float> samples;
};
enum class ParseError { none, signature, truncated, protocol, codec, reserved,
                        sample_rate, channels, sample_type, length, stream_name, nonfinite };
ParseError decode(const uint8_t *data, size_t size, Packet &out);
const char *describe(ParseError error);
const char *format_name(uint8_t type);
bool read_stream_name(const uint8_t *data, size_t size, std::string &name);
bool valid_stream_name(const std::string &name);
uint64_t frames_to_ns(uint64_t frames, uint32_t rate);
}
