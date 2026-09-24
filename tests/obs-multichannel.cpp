// SPDX-License-Identifier: GPL-2.0-or-later
// Real DLL -> UDP receiver -> jitter buffer -> OBS planar audio, with distinct lanes.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <obs.h>
#include <obs-audio-controls.h>
#include <QApplication>
#include <QAction>
#include <QDialog>
#include <QLabel>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWidget>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "frontend-stub.hpp"

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static float lane(unsigned channel) { return (channel % 2 ? -1.f : 1.f) * float(channel + 1) / 32.f; }
struct Capture {
    unsigned channels;
    unsigned planes;
    std::atomic<unsigned> matching{0};
    std::atomic<bool> backwards{false};
    uint64_t last = 0;
};
static void capture(void *opaque, obs_source_t *, const audio_data *data, bool) {
    auto &state = *static_cast<Capture *>(opaque);
    if (data->timestamp < state.last) state.backwards = true;
    state.last = data->timestamp;
    bool match = data->frames != 0;
    for (unsigned c = 0; c < state.planes && match; ++c) {
        if (!data->data[c]) { match = false; break; }
        const auto *samples = reinterpret_cast<const float *>(data->data[c]);
        const float expected = c < state.channels ? lane(c) : 0.f;
        for (uint32_t f = 0; f < data->frames; ++f)
            if (std::abs(samples[f] - expected) > .00001f) { match = false; break; }
    }
    if (match) ++state.matching;
}
static std::vector<uint8_t> packet(uint32_t sequence, unsigned channels, unsigned type) {
    // One millisecond fits even 8-channel PCM24 inside VBAN's 1464-byte limit.
    constexpr unsigned frames = 48;
    const unsigned width = type == 2 ? 3 : 2;
    std::vector<uint8_t> bytes(28 + frames * channels * width);
    std::memcpy(bytes.data(), "VBAN", 4);
    bytes[4] = 3; bytes[5] = frames - 1; bytes[6] = uint8_t(channels - 1); bytes[7] = uint8_t(type);
    std::memcpy(bytes.data() + 8, "MULTICHANNEL", 12);
    for (unsigned b = 0; b < 4; ++b) bytes[24 + b] = uint8_t(sequence >> (8 * b));
    for (unsigned f = 0; f < frames; ++f)
        for (unsigned c = 0; c < channels; ++c) {
            const auto sample = static_cast<uint32_t>(static_cast<int32_t>(lane(c) * (type == 2 ? 8388608.f : 32768.f)));
            for (unsigned b = 0; b < width; ++b)
                bytes[28 + (f * channels + c) * width + b] = uint8_t(sample >> (8 * b));
        }
    return bytes;
}
int main(int argc, char **argv) {
    try {
        check(argc == 5, "Usage: obs-multichannel plugin.dll data-dir config-dir channels");
        const unsigned channels = unsigned(std::stoi(argv[4]));
        check(channels >= 1 && channels <= 8, "Channel count is 1..8");
        constexpr speaker_layout layouts[]{SPEAKERS_UNKNOWN, SPEAKERS_MONO, SPEAKERS_STEREO,
            SPEAKERS_2POINT1, SPEAKERS_4POINT0, SPEAKERS_4POINT1, SPEAKERS_5POINT1, SPEAKERS_7POINT1, SPEAKERS_7POINT1};
        qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts");
        QApplication app(argc, argv);
        QWidget window;
        auto *frontend = new TestFrontend(window);
        obs_frontend_set_callbacks_internal(frontend);
        WSADATA wsa{};
        check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock startup");
        SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        SOCKET reservation = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        check(sender != INVALID_SOCKET && reservation != INVALID_SOCKET, "Create sockets");
        sockaddr_in destination{};
        destination.sin_family = AF_INET; destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(reservation, reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) == 0, "Reserve receiver port");
        int size = sizeof(destination);
        check(getsockname(reservation, reinterpret_cast<sockaddr *>(&destination), &size) == 0, "Read receiver port");
        closesocket(reservation);
        const QString root = QString::fromLocal8Bit(argv[3]);
        check(QDir().mkpath(root + "/obs-vban-audio"), "Create isolated settings folder");
        QFile file(root + "/obs-vban-audio/settings.json");
        check(file.open(QIODevice::WriteOnly), "Write isolated settings");
        QJsonArray slots{QJsonObject{{"enabled", true}, {"label", "Multichannel"},
            {"sender_ip", "127.0.0.1"}, {"stream_name", "MULTICHANNEL"}}};
        file.write(QJsonDocument(QJsonObject{{"version", 1}, {"common_ip", true},
            {"sender_ip", "127.0.0.1"}, {"port", ntohs(destination.sin_port)}, {"slots", slots}}).toJson());
        file.close();
        check(obs_startup("en-US", root.toUtf8().constData(), nullptr), "OBS startup");
        obs_audio_info audio{}; audio.samples_per_sec = 48000; audio.speakers = layouts[channels];
        check(obs_reset_audio(&audio), "OBS multichannel audio startup");
        obs_module_t *module = nullptr;
        const auto dll_path = QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[1])).toUtf8();
        check(obs_open_module(&module, dll_path.constData(), argv[2]) == MODULE_SUCCESS && obs_init_module(module), "Load plugin DLL");
        obs_data_t *settings = obs_data_create(); obs_data_set_int(settings, "slot", 0);
        auto *source = obs_source_create("vban_audio_input", "Multichannel", settings, nullptr);
        obs_data_release(settings);
        check(source != nullptr, "Create source with no channel-count setting");
        Capture state{channels, channels == 7 ? 8 : channels};
        obs_source_add_audio_capture_callback(source, capture, &state);
        obs_source_inc_active(source);
        auto *meter = obs_volmeter_create(OBS_FADER_LOG);
        check(meter && obs_volmeter_attach_source(meter, source), "Attach native OBS multichannel meter");
        frontend->action->trigger(); app.processEvents();
        auto *dialog = window.findChild<QDialog *>();
        check(dialog, "Open settings with live input indicators");
        auto *input_channels = dialog->findChild<QLabel *>("input_channels_0");
        auto *disabled_channels = dialog->findChild<QLabel *>("input_channels_1");
        auto *receiving = dialog->findChild<QLabel *>("receiving_streams");
        auto *input_format = dialog->findChild<QLabel *>("input_format_0");
        check(input_channels && disabled_channels && receiving && input_format, "Input indicators exist");
        check(input_channels->text() == QString::fromUtf8("—") &&
              disabled_channels->text() == QString::fromUtf8("—"), "Waiting and disabled streams have no live channel count");
        check(receiving->text() == "Receiving: 0 / 8 streams", "No configured stream is counted until packets arrive");
        uint32_t sequence = 0;
        auto send = [&](unsigned count, unsigned type, unsigned milliseconds) {
            auto deadline = std::chrono::steady_clock::now();
            for (unsigned ms = 0; ms < milliseconds; ++ms) {
                const auto bytes = packet(sequence++, count, type);
                check(sendto(sender, reinterpret_cast<const char *>(bytes.data()), int(bytes.size()), 0,
                    reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) == int(bytes.size()), "Send full datagram");
                app.processEvents(); deadline += std::chrono::milliseconds(1); std::this_thread::sleep_until(deadline);
            }
        };
        // Switch channel count on the SAME stream and source, without reopening properties.
        send(channels == 2 ? 1 : 2, 1, 150);
        for (unsigned type : {1U, 2U}) {
            const auto before = state.matching.load();
            send(channels, type, 700);
            check(state.matching.load() > before + 20, "Every sample in every OBS lane matches its own signed channel marker");
            check(input_channels->text() == QString::number(channels), "Live UI reports the incoming count after a channel change");
            check(receiving->text() == "Receiving: 1 / 8 streams", "Multichannel packets still count as one incoming stream");
            check(input_format->text() == (type == 1 ? "PCM 16-bit" : "PCM 24-bit"), "Live input format follows the actual received bit depth");
            check(obs_volmeter_get_nr_channels(meter) == int(state.planes), "Native OBS meter exposes every lane allowed by its audio layout");
        }
        check(!state.backwards, "Timestamps remain monotonic across format changes");
        if (channels == 8) dialog->grab().save(root + "/input-indicators.png");
        const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (receiving->text() != "Receiving: 0 / 8 streams" && std::chrono::steady_clock::now() < timeout) {
            app.processEvents(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(receiving->text() == "Receiving: 0 / 8 streams" && input_channels->text() == QString::fromUtf8("—") && input_format->text() == QString::fromUtf8("—"),
              "Stopped streams clear the live count and do not retain a stale channel indicator");
        obs_volmeter_destroy(meter);
        obs_source_remove_audio_capture_callback(source, capture, &state);
        obs_source_dec_active(source); obs_source_release(source);
        obs_shutdown(); app.processEvents(); obs_frontend_set_callbacks_internal(nullptr);
        closesocket(sender); WSACleanup();
        std::cout << "OBS multichannel passed: " << channels << " incoming channels, PCM16/24, distinct signed lanes, "
            << state.matching << " matching blocks, automatic live channel change, input indicators, native meter lanes, stream timeout"
            << (channels == 7 ? ", silent eighth lane" : "") << ".\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "OBS multichannel failed: " << e.what() << "\n"; return 1;
    }
}
