// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.hpp"
#include <obs-module.h>
#include <util/bmem.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QIcon>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace vban {
static QString config_path() {
    char *path = obs_module_config_path("settings.json");
    const QString result = QString::fromUtf8(path ? path : "");
    bfree(path);
    return result;
}
Config read_config(std::string &error) {
    Config cfg;
    const auto path = config_path();
    if (path.isEmpty()) { error = "OBS module configuration path is unavailable."; return cfg; }
    QFile file(path);
    if (!file.exists()) return cfg;
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString().toStdString(); return cfg; }
    QJsonParseError parse{};
    auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        error = "Could not read VBAN settings: " + parse.errorString().toStdString();
        return cfg;
    }
    const auto root = doc.object();
    if (root.value("version").toInt() != 1) { error = "Unsupported VBAN settings version."; return cfg; }
    cfg.common_ip = root.value("common_ip").toBool(true);
    cfg.sender_ip = root.value("sender_ip").toString().toStdString();
    const int port = root.value("port").toInt(6980);
    if (port < 1 || port > 65535) { error = "Saved UDP port is invalid."; return cfg; }
    cfg.port = static_cast<uint16_t>(port);
    const auto slots = root.value("slots").toArray();
    for (int i = 0; i < std::min(slots.size(), qsizetype(slot_count)); ++i) {
        const auto s = slots[i].toObject();
        cfg.slots[i] = {s.value("enabled").toBool(), s.value("label").toString().toStdString(),
            s.value("sender_ip").toString().toStdString(), s.value("stream_name").toString().toStdString()};
    }
    const int buffer = root.value("return_buffer_ms").toInt(default_return_buffer_ms);
    cfg.return_buffer_ms = buffer >= int(min_return_buffer_ms) && buffer <= int(max_return_buffer_ms)
        ? static_cast<uint32_t>(buffer) : 0;
    cfg.return_local_ip = root.value("return_local_ip").toString().trimmed().toStdString();
    const auto returns = root.value("returns").toArray();
    for (int i = 0; i < std::min(returns.size(), qsizetype(return_count)); ++i) {
        const auto item = returns[i].toObject();
        auto &r = cfg.returns[i];
        r.enabled = item.value("enabled").toBool(false);
        r.destination_ip = item.value("destination_ip").toString().toStdString();
        const auto destination_port = item.value("destination_port").toInt(6980);
        r.destination_port = destination_port > 0 && destination_port <= 65535
            ? static_cast<uint16_t>(destination_port) : 0;
        r.stream_name = item.value("stream_name").toString(QString::fromStdString(r.stream_name)).toStdString();
        r.pcm_bits = item.value("pcm_bits").toInt(24);
    }
    return cfg;
}
bool write_config(const Config &cfg, std::string &error) {
    const auto path = config_path();
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = "Cannot create the OBS module settings directory."; return false;
    }
    QJsonArray slots;
    for (const auto &s : cfg.slots) slots.append(QJsonObject{
        {"enabled", s.enabled}, {"label", QString::fromStdString(s.label)},
        {"sender_ip", QString::fromStdString(s.sender_ip)}, {"stream_name", QString::fromStdString(s.stream_name)}});
    QJsonArray returns;
    for (const auto &r : cfg.returns) returns.append(QJsonObject{
        {"enabled", r.enabled}, {"destination_ip", QString::fromStdString(r.destination_ip)},
        {"destination_port", r.destination_port}, {"stream_name", QString::fromStdString(r.stream_name)},
        {"pcm_bits", r.pcm_bits}});
    const QJsonDocument doc(QJsonObject{
        {"version", 1}, {"common_ip", cfg.common_ip}, {"sender_ip", QString::fromStdString(cfg.sender_ip)},
        {"port", cfg.port}, {"slots", slots}, {"returns", returns},
        {"return_local_ip", QString::fromStdString(cfg.return_local_ip)}, {"return_buffer_ms", int(cfg.return_buffer_ms)}});
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { error = file.errorString().toStdString(); return false; }
    const auto bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        error = "Could not save settings: " + file.errorString().toStdString(); return false;
    }
    return true;
}
std::string status_text(const Receiver &receiver, int index) {
    if (index < 0 || index >= int(slot_count)) return "Choose a configured stream, or open VBAN settings.";
    const auto cfg = receiver.config();
    const auto s = receiver.status(static_cast<size_t>(index));
    QString text = QString("Status: %1<br>Sender: %2 &nbsp; UDP port: %3<br>Stream: %4")
        .arg(state_name(s.state), QString::fromStdString(cfg.ip(index)).toHtmlEscaped())
        .arg(cfg.port).arg(QString::fromStdString(cfg.slots[index].stream_name).toHtmlEscaped());
    if (s.format.rate) text += QString("<br>Format: %1 Hz / %2 ch / %3%4")
        .arg(s.format.rate).arg(s.format.channels).arg(format_name(s.format.type))
        .arg(s.format.channels == 7 ? " (channel 8 padded with silence)" : "");
    text += QString("<br>Packets: %1 &nbsp; Lost: %2 &nbsp; Duplicate: %3<br>"
                    "Reordered: %4 &nbsp; Late: %5 &nbsp; Invalid: %6 &nbsp; Unsupported: %7<br>"
                    "Underruns: %8 &nbsp; Overruns: %9 &nbsp; Clock corrections: %10")
        .arg(s.counters.received).arg(s.counters.lost).arg(s.counters.duplicates)
        .arg(s.counters.reordered).arg(s.counters.late).arg(s.counters.corrupt)
        .arg(s.counters.unsupported).arg(s.counters.underruns).arg(s.counters.overruns)
        .arg(s.counters.drift_corrections);
    if (!s.error.empty()) text += "<br>" + QString::fromStdString(s.error).toHtmlEscaped();
    return text.toStdString();
}
class SettingsDialog final : public QDialog {
public:
    SettingsDialog(QWidget *parent, std::shared_ptr<Receiver> receiver, Config initial,
                   std::shared_ptr<MonitorReturn> returns, std::function<bool(const Config &, std::string &)> apply)
        : QDialog(parent), receiver_(std::move(receiver)), returns_(std::move(returns)), apply_(std::move(apply)) {
        setWindowTitle("VBAN Audio Settings");
        char *icon = obs_module_file("vban-audio.png");
        if (icon) { setWindowIcon(QIcon(QString::fromUtf8(icon))); bfree(icon); }
        setAttribute(Qt::WA_DeleteOnClose);
        setMinimumWidth(900);
        auto *layout = new QVBoxLayout(this);
        common_ = new QCheckBox("Use one sender IP for all streams", this);
        common_->setObjectName("common_ip"); common_->setChecked(initial.common_ip);
        layout->addWidget(common_);
        auto *form = new QFormLayout;
        ip_ = new QLineEdit(QString::fromStdString(initial.sender_ip), this);
        ip_->setPlaceholderText("192.168.1.50");
        ip_->setMaxLength(15);
        form->addRow("Common sender IPv4", ip_);
        port_ = new QSpinBox(this); port_->setRange(1,65535); port_->setValue(initial.port);
        form->addRow("UDP listen port", port_);
        layout->addLayout(form);
        auto *grid = new QGridLayout;
        const char *headers[]{"Slot", "Enabled", "Friendly name", "VBAN stream name", "Sender IPv4", "Live status"};
        for (int c = 0; c < 6; ++c) grid->addWidget(new QLabel(headers[c], this), 0, c);
        for (size_t i = 0; i < slot_count; ++i) {
            const auto &s = initial.slots[i];
            const int row = static_cast<int>(i)+1;
            grid->addWidget(new QLabel(QString::number(row), this), row, 0);
            enabled_[i] = new QCheckBox(this); enabled_[i]->setChecked(s.enabled);
            labels_[i] = new QLineEdit(QString::fromStdString(s.label), this);
            labels_[i]->setMaxLength(64);
            names_[i] = new QLineEdit(QString::fromStdString(s.stream_name), this);
            names_[i]->setObjectName(QString("stream_%1").arg(i)); names_[i]->setMaxLength(16); names_[i]->setPlaceholderText("MIC");
            senders_[i] = new QLineEdit(QString::fromStdString(s.sender_ip), this);
            senders_[i]->setObjectName(QString("sender_%1").arg(i)); senders_[i]->setMaxLength(15);
            states_[i] = new QLabel(this);
            grid->addWidget(enabled_[i], row, 1);
            grid->addWidget(labels_[i], row, 2);
            grid->addWidget(names_[i], row, 3);
            grid->addWidget(senders_[i], row, 4);
            grid->addWidget(states_[i], row, 5);
        }
        layout->addLayout(grid);
        auto *help = new QLabel("Stream names are case-sensitive (1–16 printable ASCII characters). "
            "Live status reflects the applied settings. Multiple OBS sources can select the same slot.", this);
        help->setWordWrap(true); layout->addWidget(help);
        auto *returns_box = new QGroupBox("VBAN RETURNS", this);
        auto *returns_layout = new QVBoxLayout(returns_box);
        auto *network_form = new QFormLayout;
        return_local_ = new QComboBox(returns_box);
        return_local_->setObjectName("return_local_ip");
        return_local_->addItem("Automatic (Windows route)", QString());
        std::string adapter_error;
        for (const auto &adapter : local_ipv4_addresses(adapter_error))
            return_local_->addItem(QString::fromStdString(adapter.address + " - " + adapter.adapter_name),
                                   QString::fromStdString(adapter.address));
        if (!initial.return_local_ip.empty()) {
            auto index = return_local_->findData(QString::fromStdString(initial.return_local_ip));
            if (index < 0) {
                return_local_->addItem(QString::fromStdString(initial.return_local_ip + " (unavailable)"),
                                       QString::fromStdString(initial.return_local_ip));
                index = return_local_->count() - 1;
            }
            return_local_->setCurrentIndex(index);
        }
        return_local_->setToolTip(adapter_error.empty()
            ? "Choose this OBS computer's LAN address. Both returns use its adapter. Addresses refresh when this window opens."
            : QString::fromStdString(adapter_error));
        network_form->addRow("Send from this PC", return_local_);
        return_buffer_ = new QSpinBox(returns_box);
        return_buffer_->setObjectName("return_buffer_ms");
        return_buffer_->setRange(min_return_buffer_ms, max_return_buffer_ms);
        return_buffer_->setSingleStep(10); return_buffer_->setSuffix(" ms");
        return_buffer_->setValue(initial.return_buffer_ms ? initial.return_buffer_ms : default_return_buffer_ms);
        return_buffer_->setToolTip("Wait for source audio before sending. Start at 60 ms; try 100 ms if Late audio frames keep increasing. Higher values add monitor delay. Network reception buffering is set in VoiceMeeter.");
        network_form->addRow("Return audio buffer", return_buffer_);
        returns_layout->addLayout(network_form);
        auto *returns_grid = new QGridLayout;
        returns_layout->addLayout(returns_grid);
        const char *return_headers[]{"Return", "Enabled", "PCM format", "Destination IPv4", "UDP port", "VBAN stream name", "Status"};
        for (int c = 0; c < 7; ++c) returns_grid->addWidget(new QLabel(return_headers[c], returns_box), 0, c);
        for (size_t i = 0; i < return_count; ++i) {
            const auto &r = initial.returns[i];
            const int row = static_cast<int>(i) + 1;
            const auto prefix = QString("return_%1_").arg(i);
            return_enabled_[i] = new QCheckBox(returns_box);
            return_enabled_[i]->setObjectName(prefix + "enabled"); return_enabled_[i]->setChecked(r.enabled);
            return_formats_[i] = new QComboBox(returns_box);
            return_formats_[i]->setObjectName(prefix + "pcm_bits");
            return_formats_[i]->addItem("PCM 16-bit", 16);
            return_formats_[i]->addItem("PCM 24-bit", 24);
            return_formats_[i]->setCurrentIndex(return_formats_[i]->findData(r.pcm_bits == 16 ? 16 : 24));
            return_formats_[i]->setToolTip("Choose this return's PCM bit depth. The sample rate follows OBS (48 kHz when OBS is set to 48 kHz). Click Apply to use the selection.");
            return_ips_[i] = new QLineEdit(QString::fromStdString(r.destination_ip), returns_box);
            return_ips_[i]->setObjectName(prefix + "ip"); return_ips_[i]->setPlaceholderText("192.168.1.50");
            return_ports_[i] = new QSpinBox(returns_box);
            return_ports_[i]->setObjectName(prefix + "port"); return_ports_[i]->setRange(1, 65535);
            return_ports_[i]->setValue(r.destination_port ? r.destination_port : 6980);
            return_names_[i] = new QLineEdit(QString::fromStdString(r.stream_name), returns_box);
            return_names_[i]->setObjectName(prefix + "name");
            return_states_[i] = new QLabel(returns_box);
            return_states_[i]->setObjectName(prefix + "status");
            returns_grid->addWidget(new QLabel(QString("RETURN %1").arg(i+1), returns_box), row, 0);
            returns_grid->addWidget(return_enabled_[i], row, 1);
            returns_grid->addWidget(return_formats_[i], row, 2);
            returns_grid->addWidget(return_ips_[i], row, 3);
            returns_grid->addWidget(return_ports_[i], row, 4);
            returns_grid->addWidget(return_names_[i], row, 5);
            auto *status_layout = new QVBoxLayout;
            status_layout->addWidget(return_states_[i]);
            return_routes_[i] = new QLabel(returns_box);
            return_routes_[i]->setObjectName(prefix + "route");
            return_routes_[i]->setTextFormat(Qt::PlainText);
            return_routes_[i]->setWordWrap(true);
            return_routes_[i]->setMinimumWidth(200);
            return_routes_[i]->setMinimumHeight(2 * return_routes_[i]->fontMetrics().lineSpacing());
            status_layout->addWidget(return_routes_[i]);
            returns_grid->addLayout(status_layout, row, 6);
        }
        auto *return_help = new QLabel("Both returns send the same OBS headphone/monitor mix. "
            "Use Monitor Only or Monitor and Output in OBS to include a source. "
            "Do not route the returns back into the VBAN feeds entering OBS.", returns_box);
        return_help->setWordWrap(true); returns_grid->addWidget(return_help, 3, 0, 1, 7);
        layout->addWidget(returns_box);
        error_ = new QLabel(this); error_->setWordWrap(true); error_->setTextFormat(Qt::PlainText);
        layout->addWidget(error_);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Apply | QDialogButtonBox::Ok, this);
        auto *donate = buttons->addButton("Donate on Ko-fi", QDialogButtonBox::ActionRole);
        donate->setObjectName("donate");
        donate->setToolTip("Support development at https://ko-fi.com/msffixit");
        connect(donate, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(QUrl("https://ko-fi.com/msffixit"));
        });
        layout->addWidget(buttons);
        connect(common_, &QCheckBox::toggled, this, [this] { update_mode(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { if (this->apply()) accept(); });
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { this->apply(); });
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { refresh(); });
        timer->start(500);
        update_mode(); refresh();
    }
private:
    void update_mode() {
        ip_->setEnabled(common_->isChecked());
        for (auto *sender : senders_) sender->setEnabled(!common_->isChecked());
    }
    void refresh() {
        for (size_t i = 0; i < slot_count; ++i) {
            auto s = receiver_->status(i);
            states_[i]->setText(QString::fromUtf8(state_name(s.state)));
            states_[i]->setToolTip(QString::fromStdString(status_text(*receiver_, static_cast<int>(i))));
        }
        for (size_t i = 0; i < return_count; ++i) {
            const auto s = returns_->status(i);
            QString text = return_state_name(s.state);
            if (s.state == ReturnState::sending && s.silence) text += " (silence)";
            return_states_[i]->setText(text);
            const auto route = s.source_ip.empty() ? QString() :
                QString("%1 -> %2:%3").arg(QString::fromStdString(s.source_ip),
                    QString::fromStdString(s.destination_ip)).arg(s.destination_port);
            return_routes_[i]->setText(route);
            auto diagnostic = QString(
                "Applied stream: %1\nSocket source: %2:%3\nDestination: %4:%5\n"
                "Packets sent: %6\nSocket errors: %7\nLast send: %8 ms ago\n"
                "Send gaps over 20 ms: %9\nLargest send gap: %10 ms\n"
                "Capture queue overflows: %11\nLate audio frames: %12\n"
                "Audio counters cover current source lifetimes; startup/monitor changes can add late frames.\n"
                "Socket source is before any router/NAT translation.\n%13")
                .arg(QString::fromStdString(s.stream_name), QString::fromStdString(s.source_ip))
                .arg(s.source_port).arg(QString::fromStdString(s.destination_ip)).arg(s.destination_port)
                .arg(s.packets).arg(s.errors).arg(s.last_send_age_ms, 0, 'f', 1)
                .arg(s.send_gaps).arg(s.max_send_gap_ms, 0, 'f', 1)
                .arg(s.capture_drops).arg(s.late_audio_frames).arg(QString::fromStdString(s.detail));
            diagnostic += QString("\nApplied format: Stereo PCM %1-bit").arg(s.pcm_bits);
            diagnostic += QString("\nReturn buffer: %1 ms\nCapture queue peak: %2 / %3 blocks (busiest source)\n"
                "Clock corrections: %4\nClock discontinuities: %5\nClipped samples: %6\nInvalid float samples: %7\n"
                "Windows audio priority: %8\nClipping means the combined mix exceeds full scale; lower source faders.")
                .arg(s.buffer_ms).arg(s.capture_queue_peak).arg(s.capture_queue_capacity)
                .arg(s.clock_corrections).arg(s.clock_discontinuities).arg(s.clipped_samples)
                .arg(s.nonfinite_samples).arg(s.audio_priority ? "Active" : "Unavailable");
            return_states_[i]->setToolTip(diagnostic);
            return_routes_[i]->setToolTip(diagnostic);
        }
    }
    bool apply() {
        Config cfg;
        cfg.return_buffer_ms = static_cast<uint32_t>(return_buffer_->value());
        cfg.return_local_ip = return_local_->currentData().toString().toStdString();
        cfg.common_ip = common_->isChecked();
        cfg.sender_ip = ip_->text().trimmed().toStdString();
        cfg.port = static_cast<uint16_t>(port_->value());
        for (size_t i = 0; i < slot_count; ++i) cfg.slots[i] = {
            enabled_[i]->isChecked(), labels_[i]->text().toStdString(),
            senders_[i]->text().trimmed().toStdString(), names_[i]->text().toStdString()};
        for (size_t i = 0; i < return_count; ++i) cfg.returns[i] = {
            return_enabled_[i]->isChecked(), return_ips_[i]->text().trimmed().toStdString(),
            static_cast<uint16_t>(return_ports_[i]->value()), return_names_[i]->text().toStdString(),
            return_formats_[i]->currentData().toInt()};
        std::string error;
        try {
            if (!apply_(cfg, error)) {
                error_->setText(QString::fromStdString(error)); return false;
            }
            error_->setText("Settings saved.");
            refresh(); return true;
        } catch (const std::exception &e) { error_->setText(QString::fromUtf8(e.what())); return false; }
    }
    std::shared_ptr<Receiver> receiver_;
    std::shared_ptr<MonitorReturn> returns_;
    std::function<bool(const Config &, std::string &)> apply_;
    QCheckBox *common_{};
    QLineEdit *ip_{};
    QSpinBox *port_{}, *return_buffer_{};
    QLabel *error_{};
    std::array<QCheckBox *, slot_count> enabled_{};
    std::array<QLineEdit *, slot_count> labels_{}, names_{}, senders_{};
    std::array<QLabel *, slot_count> states_{};
    std::array<QCheckBox *, return_count> return_enabled_{};
    std::array<QLineEdit *, return_count> return_ips_{}, return_names_{};
    std::array<QSpinBox *, return_count> return_ports_{};
    std::array<QComboBox *, return_count> return_formats_{};
    std::array<QLabel *, return_count> return_states_{}, return_routes_{};
    QComboBox *return_local_{};
};
QDialog *make_settings_dialog(QWidget *parent, std::shared_ptr<Receiver> receiver,
                             Config initial, std::shared_ptr<MonitorReturn> returns,
                             std::function<bool(const Config &, std::string &)> apply) {
    return new SettingsDialog(parent, std::move(receiver), std::move(initial), std::move(returns), std::move(apply));
}
}
