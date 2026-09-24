// SPDX-License-Identifier: GPL-2.0-or-later
#include <winsock2.h>
#include <ws2tcpip.h>
#include <obs.h>
#include <obs-properties.h>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QThread>
#include <QWidget>
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
#include "frontend-stub.hpp"

static void check(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
static std::atomic<int> captures{0};
static std::atomic<bool> backwards{false};
struct Capture { uint64_t last=0; std::atomic<int> count{0}; };
static void capture(void *param,obs_source_t *,const audio_data *data,bool) {
    auto &state=*static_cast<Capture*>(param);
    if(data->timestamp < state.last) backwards=true;
    state.last=data->timestamp;
    if(data->frames && data->data[0] && reinterpret_cast<const float*>(data->data[0])[0]>.01f) {
        ++state.count;++captures;
    }
}
static std::vector<uint8_t> packet(uint32_t seq,const std::string &name,uint8_t channels=2,uint8_t type=1) {
    const int width=type==2?3:2;
    constexpr int frames=96;
    std::vector<uint8_t> p(28+frames*channels*width,0);
    std::memcpy(p.data(),"VBAN",4);p[4]=3;p[5]=frames-1;p[6]=channels-1;p[7]=type;
    std::memcpy(p.data()+8,name.data(),name.size());
    std::memcpy(p.data()+24,&seq,4);
    for(size_t i=28;i<p.size();i+=width)p[i+width-1]=0x20;
    return p;
}
int main(int argc,char **argv) {
    try {
        check(argc==4,"Usage: obs-smoke plugin.dll data-dir config-dir");
        qputenv("QT_QPA_PLATFORM","minimal:enable_fonts");
        QApplication app(argc,argv);
        QWidget window;
        auto *frontend = new TestFrontend(window); // OBS takes ownership.
        obs_frontend_set_callbacks_internal(frontend);
        const QString root=QString::fromLocal8Bit(argv[3]);
        QDir().mkpath(root+"/obs-vban-audio");
        WSADATA wsa{};check(WSAStartup(MAKEWORD(2,2),&wsa)==0,"Winsock startup");
        SOCKET sender=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        check(sender!=INVALID_SOCKET,"Sender socket");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        check(bind(sender,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Sender bind");
        int size=sizeof(address);getsockname(sender,reinterpret_cast<sockaddr*>(&address),&size);
        // Find an unused receiving port independently from the sender's source port.
        SOCKET reservation=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in destination{};destination.sin_family=AF_INET;destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        check(bind(reservation,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==0,"Port reservation");
        size=sizeof(destination);getsockname(reservation,reinterpret_cast<sockaddr*>(&destination),&size);
        closesocket(reservation);
        QJsonArray slots;
        for(int i=0;i<8;++i)slots.append(QJsonObject{{"enabled",true},{"label",QString("Stream %1").arg(i+1)},
            {"sender_ip","127.0.0.1"},{"stream_name",QString("S%1").arg(i)}});
        QFile file(root+"/obs-vban-audio/settings.json");
        check(file.open(QIODevice::WriteOnly),"Create isolated settings");
        file.write(QJsonDocument(QJsonObject{{"version",1},{"common_ip",true},{"sender_ip","127.0.0.1"},
            {"port",ntohs(destination.sin_port)},{"slots",slots}}).toJson());
        file.close();
        check(obs_startup("en-US",root.toUtf8().constData(),nullptr),"OBS startup");
        obs_audio_info audio{};audio.samples_per_sec=48000;audio.speakers=SPEAKERS_STEREO;
        check(obs_reset_audio(&audio),"OBS audio startup");
        obs_module_t *module=nullptr;
        check(obs_open_module(&module,argv[1],argv[2])==MODULE_SUCCESS,"Load plugin DLL");
        check(obs_init_module(module),"Initialize plugin DLL");
        check(frontend->action && frontend->action->text()=="VBAN Stream Settings","Tools menu registration");
        obs_data_t *settings=obs_data_create();obs_data_set_int(settings,"slot",0);
        obs_source_t *first=obs_source_create("vban_audio_input","Test Mic",settings,nullptr);
        obs_source_t *second=obs_source_create("vban_audio_input","Test Duplicate",settings,nullptr);
        check(first && second,"Create native OBS sources");
        check(obs_source_get_output_flags(first)&OBS_SOURCE_AUDIO,"Source is audio input");
        // Default source names follow friendly labels; user names survive selection changes.
        auto *automatic = obs_source_create("vban_audio_input", "VBAN Stream", nullptr, nullptr);
        auto *duplicate = obs_source_create("vban_audio_input", "VBAN Stream 2", nullptr, nullptr);
        check(automatic && duplicate, "Create sources for naming checks");
        check(std::string(obs_source_get_name(automatic)) == "VBAN Stream", "Unselected source keeps its name");
        obs_source_update(automatic, settings);
        obs_source_update(duplicate, settings);
        auto *legacy = obs_source_create("vban_audio_input", "VBAN Audio 3", nullptr, nullptr);
        check(legacy, "Load an unselected legacy source name");
        obs_source_update(legacy, settings);
        check(std::string(obs_source_get_name(legacy)).find("Stream 1") == 0, "Legacy default name follows its friendly label");
        obs_source_release(legacy);

        check(std::string(obs_source_get_name(automatic)) == "Stream 1", "Fader uses friendly label, not VBAN stream name");
        check(std::string(obs_source_get_name(duplicate)) == "Stream 1 2", "Duplicate fader names remain unique");
        obs_data_set_int(settings, "slot", 1);
        obs_source_update(automatic, settings);
        check(std::string(obs_source_get_name(automatic)) == "Stream 2", "Automatic name follows new selection");
        obs_source_update(first, settings);
        check(std::string(obs_source_get_name(first)) == "Test Mic", "Initial custom fader name preserved");
        auto *saved_source = obs_save_source(automatic);
        obs_source_release(automatic);
        obs_wait_for_destroy_queue();
        automatic = obs_load_source(saved_source);
        obs_data_release(saved_source);
        check(automatic, "Restore saved source");
        obs_data_set_int(settings, "slot", 0);
        obs_source_update(automatic, settings);
        check(std::string(obs_source_get_name(automatic)) == "Stream 1", "Automatic naming survives source save and restore");
        obs_source_set_name(automatic, "My fader");
        obs_data_set_int(settings, "slot", 1);
        obs_source_update(automatic, settings);
        check(std::string(obs_source_get_name(automatic)) == "My fader", "Manual rename preserved after selecting another stream");
        obs_source_release(automatic);
        obs_source_release(duplicate);
        obs_data_set_int(settings, "slot", 0);
        obs_source_update(first, settings);
        int property_refreshes = 0;
        auto count_refresh = [](void *param, calldata_t *) { ++*static_cast<int *>(param); };
        signal_handler_connect(obs_source_get_signal_handler(first), "update_properties", count_refresh, &property_refreshes);
        Capture a,b;
        obs_source_add_audio_capture_callback(first,capture,&a);
        obs_source_add_audio_capture_callback(second,capture,&b);
        obs_source_inc_active(first);obs_source_inc_active(second);
        obs_properties_t *props=obs_source_properties(first);
        check(obs_property_list_item_count(obs_properties_get(props,"slot"))==9,"All 8 slots in source properties");
        obs_properties_destroy(props);
        frontend->action->trigger();app.processEvents();
        auto dialogs=window.findChildren<QDialog*>();
        check(!dialogs.empty(),"Settings dialog opens");
        auto *dialog=dialogs.front();
        auto *stream=dialog->findChild<QLineEdit*>("stream_0");
        auto *common=dialog->findChild<QCheckBox*>("common_ip");
        auto *individual=dialog->findChild<QLineEdit*>("sender_0");
        check(stream && common && individual,"Settings controls exist");
        common->setChecked(false);check(individual->isEnabled(),"Individual IP mode");
        common->setChecked(true);check(individual->text()=="127.0.0.1" && !individual->isEnabled(),"Individual IP preserved");
        uint32_t sequence=0;
        auto send=[&](const std::string &name,int milliseconds,uint8_t channels=2,uint8_t type=1) {
            auto deadline = std::chrono::steady_clock::now();
            for(int i=0;i<milliseconds/2;++i) {
                auto bytes=packet(sequence++,name,channels,type);
                check(sendto(sender,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0,
                    reinterpret_cast<sockaddr*>(&destination),sizeof(destination))!=SOCKET_ERROR,"Send VBAN");
                app.processEvents(); deadline += std::chrono::milliseconds(2); std::this_thread::sleep_until(deadline);
            }
        };
        send("S0",1200);
        check(property_refreshes == 0, "Idle status updates do not rebuild and close the stream dropdown");
        check(a.count>10 && b.count>10,"VBAN audio reaches both real OBS capture callbacks");
        dialog->grab().save(root+"/settings-dialog.png");
        stream->setText("CHANGED");
        auto *buttons=dialog->findChild<QDialogButtonBox*>();
        buttons->button(QDialogButtonBox::Apply)->click();app.processEvents();
        check(property_refreshes == 1, "Applying global settings still refreshes the available streams");
        check(file.open(QIODevice::ReadOnly),"Read persisted settings");
        const auto saved=QJsonDocument::fromJson(file.readAll()).object();file.close();
        check(saved.value("slots").toArray()[0].toObject().value("stream_name")=="CHANGED","Settings save atomically");
        const int before=a.count;
        send("CHANGED",600,2,2);
        check(a.count>before+10,"PCM24 after live stream change reaches OBS");
        // Seven-channel input reaches the real OBS downmixer without corrupting the stream.
        const int before_surround = a.count;
        send("CHANGED",300,7,1);
        check(a.count > before_surround + 5, "Seven-channel PCM reaches OBS downmixer");
        // Churn source lifetime while the receiver worker is active.
        for(int i=0;i<40;++i) {
            auto *temporary=obs_source_create_private("vban_audio_input","Transient",settings);
            check(temporary,"Create transient source");
            obs_source_release(temporary);
            send("CHANGED",3);
        }
        stream->setText("UNSAVED");dialog->reject();app.processEvents();
        check(file.open(QIODevice::ReadOnly),"Read after Cancel");
        const auto after_cancel=QJsonDocument::fromJson(file.readAll()).object();file.close();
        check(after_cancel.value("slots").toArray()[0].toObject().value("stream_name")=="CHANGED","Cancel preserves saved settings");
        auto *source_settings=obs_source_get_settings(first);
        check(obs_data_get_int(source_settings,"slot")==0,"Source selection persisted in OBS settings");
        obs_data_release(source_settings);
        check(!backwards,"Per-source timestamps remain monotonic");
        signal_handler_disconnect(obs_source_get_signal_handler(first), "update_properties", count_refresh, &property_refreshes);
        obs_source_remove_audio_capture_callback(first,capture,&a);
        obs_source_remove_audio_capture_callback(second,capture,&b);
        obs_source_dec_active(first);obs_source_dec_active(second);
        obs_source_release(first);obs_source_release(second);obs_data_release(settings);
        obs_shutdown();app.processEvents();
        obs_frontend_set_callbacks_internal(nullptr);
        closesocket(sender);WSACleanup();
        std::cout<<"OBS DLL smoke passed: "<<captures<<" captured blocks, Tools menu, eight slots, live PCM16/24, "
                   "7-channel padding, stable properties, friendly fader names, persistent UI changes, source churn and clean unload.\n";
        return 0;
    } catch(const std::exception &e) {
        std::cerr<<"OBS smoke failed: "<<e.what()<<"\n";
        return 1;
    }
}
