// SPDX-License-Identifier: GPL-2.0-or-later
#include "vban-protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vban {
static uint64_t le(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (i * 8);
    return v;
}
bool valid_stream_name(const std::string &name) {
    return !name.empty() && name.size() <= 16 &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}
bool read_stream_name(const uint8_t *data, size_t size, std::string &name) {
    if (size < header_size || std::memcmp(data, "VBAN", 4) != 0) return false;
    size_t n = 0;
    while (n < 16 && data[8+n]) ++n;
    name.assign(reinterpret_cast<const char *>(data+8), n);
    return valid_stream_name(name);
}
ParseError decode(const uint8_t *data, size_t size, Packet &out) {
    if (size < header_size) return ParseError::truncated;
    if (std::memcmp(data, "VBAN", 4)) return ParseError::signature;
    if (data[4] & 0xe0) return ParseError::protocol;
    if (data[7] & 0xf0) return ParseError::codec;
    if (data[7] & 0x08) return ParseError::reserved;
    const auto rate_index = data[4] & 0x1f;
    if (rate_index >= sample_rates.size()) return ParseError::sample_rate;
    const unsigned channels = unsigned(data[6]) + 1;
    if (channels > 8) return ParseError::channels;
    const uint8_t type = data[7] & 7;
    if (type > 5) return ParseError::sample_type;
    constexpr size_t widths[]{1,2,3,4,4,8};
    const size_t width = widths[type];
    const unsigned frames = unsigned(data[5]) + 1;
    if (size > max_datagram || size != header_size + frames * channels * width)
        return ParseError::length;
    Packet packet;
    if (!read_stream_name(data, size, packet.name)) return ParseError::stream_name;
    packet.format = {sample_rates[rate_index], static_cast<uint8_t>(channels), type};
    packet.frames = static_cast<uint16_t>(frames);
    packet.sequence = static_cast<uint32_t>(le(data+24, 4));
    packet.samples.resize(size_t(frames) * packet.format.output_channels(), 0.0f);
    const uint8_t *p = data + header_size;
    for (unsigned f = 0; f < frames; ++f) {
        for (unsigned c = 0; c < channels; ++c, p += width) {
            const uint64_t bits = le(p, width);
            double sample = 0;
            if (type == 0) {
                sample = (double(bits) - 128.0) / 128.0;
            } else if (type <= 3) {
                const unsigned count = static_cast<unsigned>(width * 8);
                const int64_t value = (bits & (uint64_t(1) << (count-1)))
                    ? int64_t(bits) - (int64_t(1) << count) : int64_t(bits);
                sample = double(value) / double(uint64_t(1) << (count-1));
            } else if (type == 4) {
                uint32_t raw = static_cast<uint32_t>(bits);
                float value;
                std::memcpy(&value, &raw, sizeof(value));
                sample = value;
            } else {
                std::memcpy(&sample, &bits, sizeof(sample));
            }
            if (!std::isfinite(sample)) return ParseError::nonfinite;
            packet.samples[size_t(f) * packet.format.output_channels() + c] =
                static_cast<float>(std::clamp(sample, -1.0, 1.0));
        }
    }
    out = std::move(packet);
    return ParseError::none;
}
const char *format_name(uint8_t type) {
    constexpr const char *names[]{"PCM 8-bit", "PCM 16-bit", "PCM 24-bit", "PCM 32-bit", "Float32", "Float64"};
    return type < 6 ? names[type] : "Unsupported";
}
const char *describe(ParseError error) {
    switch (error) {
    case ParseError::none: return "";
    case ParseError::signature: return "Invalid VBAN signature";
    case ParseError::truncated: return "Truncated VBAN header";
    case ParseError::protocol: return "Only VBAN AUDIO is supported";
    case ParseError::codec: return "Only uncompressed PCM is supported";
    case ParseError::reserved: return "Reserved header bit is set";
    case ParseError::sample_rate: return "Invalid sample rate";
    case ParseError::channels: return "Only 1 to 8 channels are supported";
    case ParseError::sample_type: return "10-bit and 12-bit PCM are unsupported";
    case ParseError::length: return "Invalid VBAN payload length";
    case ParseError::stream_name: return "Invalid stream name";
    case ParseError::nonfinite: return "Non-finite floating point audio";
    }
    return "Invalid packet";
}
uint64_t frames_to_ns(uint64_t frames, uint32_t rate) {
    if (!rate) return 0;
    return (frames / rate) * 1000000000ULL + (frames % rate) * 1000000000ULL / rate;
}
}
