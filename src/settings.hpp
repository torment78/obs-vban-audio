// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "receiver.hpp"
#include "monitor-return.hpp"
#include <string>
class QDialog;
class QWidget;
namespace vban {
Config read_config(std::string &error);
bool write_config(const Config &config, std::string &error);
QDialog *make_settings_dialog(QWidget *parent, std::shared_ptr<Receiver> receiver,
    Config initial, std::shared_ptr<MonitorReturn> returns,
    std::function<bool(const Config &, std::string &)> apply);
std::string status_text(const Receiver &receiver, int index);
}
