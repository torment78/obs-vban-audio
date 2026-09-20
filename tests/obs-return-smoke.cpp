// SPDX-License-Identifier: GPL-2.0-or-later
#include <winsock2.h>
#include <ws2tcpip.h>
#include "vban-protocol.hpp"
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "frontend-stub.hpp"
static void check(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
struct Sink {
    SOCKET socket = INVALID_SOCKET;
    uint16_t port=0;
    std::string last_peer;
    std::vector<vban::Packet> packets;
    explicit Sink(const char *ip) {
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in address{};address.sin_family=AF_INET;InetPtonA(AF_INET,ip,&address.sin_addr);
        check(bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Bind test sink");
        int n=sizeof(address);getsockname(socket,reinterpret_cast<sockaddr*>(&address),&n);port=ntohs(address.sin_port);
        u_long mode=1;ioctlsocket(socket,FIONBIO,&mode);
    }
    ~Sink(){closesocket(socket);}
    void read(){
        std::array<uint8_t,vban::max_datagram> bytes{};
        for(;;){
            sockaddr_in peer{};int peer_size=sizeof(peer);
            const int got=recvfrom(socket,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0,
                reinterpret_cast<sockaddr*>(&peer),&peer_size);
            if(got<0)break;
            char ip[INET_ADDRSTRLEN]{};InetNtopA(AF_INET,&peer.sin_addr,ip,sizeof(ip));last_peer=ip;
            vban::Packet packet;
            check(vban::decode(bytes.data(),got,packet)==vban::ParseError::none,"Actual return is valid VBAN");
            check(packet.format.rate==48000 && packet.format.channels==2 && packet.format.type==2,"Stereo PCM24 at OBS rate");
            packets.push_back(std::move(packet));
        }
    }
    double tail_mean() const {
        double sum=0;size_t count=0;
        const auto start=packets.size()>25?packets.size()-25:0;
        for(size_t i=start;i<packets.size();++i)for(float x:packets[i].samples){sum+=x;++count;}
        return count?sum/count:0;
    }
};
static const char *source_name(void*){return "Return Test Input";}
static void *create(obs_data_t*,obs_source_t*s){return s;}
static void destroy(void*){}
static obs_audio_data *filter(void*,obs_audio_data *data){
    for(size_t c=0;c<2;++c)for(size_t i=0;i<data->frames;++i)reinterpret_cast<float*>(data->data[c])[i]*=2;
    return data;
}
static void output(obs_source_t *s,float value,uint64_t timestamp,uint32_t frames=480,uint32_t rate=48000,bool mono=false) {
    std::vector<float> samples(static_cast<size_t>(frames)*(mono?1:2),value);
    obs_source_audio audio{};audio.data[0]=reinterpret_cast<uint8_t*>(samples.data());
    audio.frames=frames;audio.speakers=mono?SPEAKERS_MONO:SPEAKERS_STEREO;
    audio.format=AUDIO_FORMAT_FLOAT;audio.samples_per_sec=rate;audio.timestamp=timestamp;
    obs_source_output_audio(s,&audio);
}
int main(int argc,char **argv){
 try {
    check(argc==4,"Expected DLL, data and isolated config paths");
    qputenv("QT_QPA_PLATFORM","minimal:enable_fonts");
    QApplication app(argc,argv);
    WSADATA wsa{};check(WSAStartup(MAKEWORD(2,2),&wsa)==0,"Winsock");
    Sink left("127.0.0.1"),right("127.0.0.2");
    QWidget window;
    auto *frontend=new TestFrontend(window);obs_frontend_set_callbacks_internal(frontend);
    const QString root=QString::fromLocal8Bit(argv[3]);
    QDir().mkpath(root+"/obs-vban-audio");
    QFile config(root+"/obs-vban-audio/settings.json");
    check(config.open(QIODevice::WriteOnly),"Isolated settings file");
    // Old configuration: no returns key. Upgrade must preserve inputs and default TX off.
    config.write(QJsonDocument(QJsonObject{{"version",1},{"common_ip",true},{"sender_ip","127.0.0.1"},
        {"port",6980},{"slots",QJsonArray{QJsonObject{{"enabled",false},{"label","Existing input"},{"stream_name","KEEP"}}}}}).toJson());
    config.close();
    check(obs_startup("en-US",root.toUtf8().constData(),nullptr),"OBS startup");
    obs_audio_info audio{};audio.samples_per_sec=48000;audio.speakers=SPEAKERS_STEREO;
    check(obs_reset_audio(&audio),"OBS audio init");
    // Avoid playing test audio through physical speakers. This affects only this test process.
    obs_set_audio_monitoring_device("Test-only unavailable device","{VBAN-RETURN-TEST-NO-DEVICE}");
    obs_source_info input{};input.id="return_test_input";input.type=OBS_SOURCE_TYPE_INPUT;
    input.output_flags=OBS_SOURCE_AUDIO;input.get_name=source_name;input.create=create;input.destroy=destroy;obs_register_source(&input);
    auto av = input; av.id = "return_test_av"; av.output_flags |= OBS_SOURCE_ASYNC_VIDEO; obs_register_source(&av);
    obs_source_info effect{};effect.id="return_test_filter";effect.type=OBS_SOURCE_TYPE_FILTER;
    effect.output_flags=OBS_SOURCE_AUDIO;effect.get_name=source_name;effect.create=create;effect.destroy=destroy;effect.filter_audio=filter;obs_register_source(&effect);
    auto *a=obs_source_create("return_test_input","Before plugin",nullptr,nullptr);
    obs_source_inc_active(a);obs_source_set_monitoring_type(a,OBS_MONITORING_TYPE_MONITOR_ONLY);
    auto *fx=obs_source_create("return_test_filter","Gain filter",nullptr,nullptr);
    obs_source_filter_add(a,fx);obs_source_release(fx);obs_source_set_volume(a,.5f);
    obs_module_t *module=nullptr;
    const auto dll_path = QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[1])).toUtf8();
    check(obs_open_module(&module,dll_path.constData(),argv[2])==MODULE_SUCCESS && obs_init_module(module),"Load return plugin");
    auto *b=obs_source_create("return_test_input","After plugin",nullptr,nullptr);obs_source_inc_active(b);
    frontend->action->trigger();app.processEvents();
    const auto dialogs=window.findChildren<QDialog*>();check(!dialogs.empty(),"Open settings");
    auto *dialog=dialogs.front();
    auto *buttons=dialog->findChild<QDialogButtonBox*>();
    auto set_return=[&](int i,bool enable){
        const auto prefix=QString("return_%1_").arg(i);
        auto *enabled=dialog->findChild<QCheckBox*>(prefix+"enabled");
        auto *ip=dialog->findChild<QLineEdit*>(prefix+"ip");
        auto *port=dialog->findChild<QSpinBox*>(prefix+"port");
        auto *name=dialog->findChild<QLineEdit*>(prefix+"name");
        check(enabled && ip && port && name,"Exactly destination controls exist");
        enabled->setChecked(enable);ip->setText(i?"127.0.0.2":"127.0.0.1");
        port->setValue(i?right.port:left.port);name->setText(i?"MONITOR-B":"MONITOR-A");
    };
    check(!dialog->findChild<QCheckBox*>("return_0_enabled")->isChecked() &&
          dialog->findChild<QLineEdit*>("return_0_name")->text()=="OBSRETURN1","Upgrade defaults both returns off");
    check(dialog->findChild<QLineEdit*>("stream_0")->text()=="KEEP","Upgrade preserves original input");
    auto *buffer=dialog->findChild<QSpinBox*>("return_buffer_ms");
    check(buffer && buffer->value()==60,"Older settings get a 60 ms return buffer");
    auto *local=dialog->findChild<QComboBox*>("return_local_ip");
    check(local && local->currentData().toString().isEmpty(),"Old configuration defaults sender adapter to Automatic");
    const auto loopback=local->findData("127.0.0.1");
    check(loopback>=0,"Dropdown enumerates active local IPv4 addresses");
    local->setCurrentIndex(loopback);
    local->showPopup();
    for(int i=0;i<7;++i){app.processEvents();std::this_thread::sleep_for(std::chrono::milliseconds(100));}
    check(local->view()->isVisible(),"Periodic status refresh keeps the adapter dropdown open");
    local->hidePopup();
    set_return(0,true);set_return(1,true);buttons->button(QDialogButtonBox::Apply)->click();
    check(config.open(QIODevice::ReadOnly),"Read saved config");auto saved=QJsonDocument::fromJson(config.readAll()).object();config.close();
    check(saved.value("return_buffer_ms").toInt()==60,"Return buffer persists");
    check(saved.value("return_local_ip").toString()=="127.0.0.1","Explicit sender IPv4 persists");
    check(saved.value("returns").toArray().size()==2 &&
          saved.value("returns").toArray()[1].toObject().value("stream_name")=="MONITOR-B","Return settings persist");
    check(saved.value("slots").toArray()[0].toObject().value("stream_name")=="KEEP","Saving returns preserves eight inputs");
    auto run=[&](int ms,float av=.1f,float bv=.2f,uint32_t b_rate=48000,bool mono=false){
        left.read();right.read();left.packets.clear();right.packets.clear();
        auto deadline=std::chrono::steady_clock::now();
        for(int i=0;i<ms/10;++i){
            const auto ts=os_gettime_ns();
            output(a,av,ts);output(b,bv,ts,b_rate/100,b_rate,mono);
            left.read();right.read();app.processEvents();
            deadline+=std::chrono::milliseconds(10);std::this_thread::sleep_until(deadline);
        }
        left.read();right.read();
    };
    auto expect=[&](double expected,const char *message){
        const auto x=left.tail_mean(),y=right.tail_mean();
        if(std::fabs(x-expected)>.018 || std::fabs(y-expected)>.018)
            std::cerr<<"Expected "<<expected<<", measured "<<x<<", "<<y<<"\n";
        check(std::fabs(x-expected)<.018 && std::fabs(y-expected)<.018,message);
    };
    run(350);expect(.1,"Existing monitored source includes post-filter audio and fader gain; off source excluded");
    check(!left.packets.empty() && !right.packets.empty(),"Both return destinations transmit");
    check(left.last_peer=="127.0.0.1" && right.last_peer=="127.0.0.1","Plugin binds both returns to dropdown address");
    for(int i=0;i<2;++i){
        const auto text=dialog->findChild<QLabel*>(QString("return_%1_route").arg(i))->text();
        check(text.startsWith("127.0.0.1 -> "),"UI shows actual applied local endpoint");
    }
    size_t same=0;
    for(const auto &p:left.packets)for(const auto &q:right.packets)if(p.sequence==q.sequence){
        check(p.name=="MONITOR-A" && q.name=="MONITOR-B" && p.samples==q.samples,"Same mixed PCM in both returns");++same;break;
    }
    check(same>20,"Compared simultaneous return payloads");
    local->addItem("Unavailable test address","192.0.2.123");
    local->setCurrentIndex(local->count()-1);
    buttons->button(QDialogButtonBox::Apply)->click();
    run(200);expect(.1,"Unavailable adapter rollback preserves working monitor mix");
    check(left.last_peer=="127.0.0.1" && right.last_peer=="127.0.0.1","Rejected adapter does not change packets");
    local->setCurrentIndex(loopback);
    obs_source_set_monitoring_type(b,OBS_MONITORING_TYPE_MONITOR_ONLY);
    run(300);expect(.3,"New monitor-only source joins dynamically");
    obs_source_set_monitoring_type(a,OBS_MONITORING_TYPE_NONE);
    run(300);expect(.2,"Monitoring off immediately removes source and queued audio");
    obs_source_set_monitoring_type(a,OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
    run(300);expect(.3,"Monitor-and-output joins the same mix");
    // Eight simultaneously monitored sources, with actual UDP reception and sequence checking.
    std::vector<obs_source_t*> extra;
    for(int i=0;i<6;++i){
        auto *source=obs_source_create("return_test_input",("Load "+std::to_string(i)).c_str(),nullptr,nullptr);
        obs_source_inc_active(source);obs_source_set_monitoring_type(source,OBS_MONITORING_TYPE_MONITOR_ONLY);
        extra.push_back(source);
    }
    left.read();right.read();left.packets.clear();right.packets.clear();
    auto next_tick=std::chrono::steady_clock::now();
    const auto audio_start=os_gettime_ns();
    for(int i=0;i<200;++i){
        // Sample timestamps advance by the audio duration, independent of callback scheduling jitter.
        const auto ts=audio_start+static_cast<uint64_t>(i)*10000000ULL;
        output(a,.1f,ts);output(b,.2f,ts);
        for(auto *source:extra)output(source,.01f,ts);
        left.read();right.read();app.processEvents();
        next_tick+=std::chrono::milliseconds(10);std::this_thread::sleep_until(next_tick);
    }
    left.read();right.read();expect(.36,"Eight monitored sources preserve expected audio under steady load");
    for(auto *sink:{&left,&right}){
        check(sink->packets.size()>600,"Sustained two-return packet delivery");
        for(size_t i=1;i<sink->packets.size();++i)
            check(sink->packets[i].sequence==sink->packets[i-1].sequence+1,"No packet sequence loss in local load test");
        for(size_t i=sink->packets.size()-100;i<sink->packets.size();++i)
            for(float sample:sink->packets[i].samples)
                if(std::fabs(sample-.36f)>=.002f){
                    std::cerr<<"Steady-load sample "<<sample<<" in packet "<<sink->packets[i].sequence<<"\n";
                    std::cerr<<dialog->findChild<QLabel*>("return_0_status")->toolTip().toStdString()<<"\n";
                    check(false,"No silent gaps or corrupt PCM during steady monitor load");
                }
    }
    // Keep sample timestamps continuous while every callback arrives 45 ms late.
    left.read();right.read();left.packets.clear();right.packets.clear();
    next_tick=std::chrono::steady_clock::now();
    const auto delayed_start=os_gettime_ns()-45000000ULL;
    for(int i=0;i<120;++i){
        const auto ts=delayed_start+static_cast<uint64_t>(i)*10000000ULL;
        output(a,.1f,ts);output(b,.2f,ts);
        for(auto *source:extra)output(source,.01f,ts);
        left.read();right.read();app.processEvents();
        next_tick+=std::chrono::milliseconds(10);std::this_thread::sleep_until(next_tick);
    }
    left.read();right.read();expect(.36,"Return buffer absorbs delayed OBS source callbacks");
    for(auto *sink:{&left,&right}) {
        check(sink->packets.size()>300,"Delayed sources retain packet delivery");
        for(size_t i=sink->packets.size()-100;i<sink->packets.size();++i)
            for(float sample:sink->packets[i].samples)
                check(std::fabs(sample-.36f)<.002f,"No gaps in actual PCM with 45 ms late callbacks");
    }
    buffer->setValue(100);buttons->button(QDialogButtonBox::Apply)->click();
    check(config.open(QIODevice::ReadOnly),"Read buffer settings");
    saved=QJsonDocument::fromJson(config.readAll()).object();config.close();
    check(saved.value("return_buffer_ms").toInt()==100,"Edited buffer persists");
    buffer->setValue(60);buttons->button(QDialogButtonBox::Apply)->click();
    for(auto *source:extra){obs_source_dec_active(source);obs_source_release(source);}
    obs_wait_for_destroy_queue();
    obs_source_set_volume(b,.25f);
    run(300);expect(.15,"Dynamic fader gain matches monitor volume");
    obs_source_set_muted(b,true);
    const bool ignores_mute=obs_get_version()>=((32u<<24)|(2u<<16));
    run(300);expect(ignores_mute?.15:.1,"Mute behavior follows installed OBS monitoring semantics");
    obs_source_set_muted(b,false);obs_source_set_volume(b,1);
    run(350,.1f,.2f,44100,true);expect(.3,"OBS resamples mono 44.1 kHz input before stereo monitor capture");
    // Editing an invalid return must not disrupt either current transmitter or input settings.
    dialog->findChild<QLineEdit*>("return_0_name")->setText("NAME-IS-TOO-LONG-17");
    buttons->button(QDialogButtonBox::Apply)->click();
    run(250);expect(.3,"Rejected return configuration leaves working audio unchanged");
    set_return(0,false);buttons->button(QDialogButtonBox::Apply)->click();
    run(250);
    check(left.packets.empty() && !right.packets.empty(),"Disable one destination independently");
    set_return(0,true);buttons->button(QDialogButtonBox::Apply)->click();
    run(250);expect(.3,"Reenable first destination without changing the second mix");
    obs_source_dec_active(b);run(250);expect(.1,"Inactive sources stop contributing");
    obs_source_inc_active(b);run(250);expect(.3,"Reactivated source resumes");
    obs_source_remove(b);run(250);expect(.1,"Removed source stops contributing even while referenced");
    obs_source_dec_active(b);obs_source_release(b);
    b=obs_source_create("return_test_av","Replacement",nullptr,nullptr);obs_source_inc_active(b);
    obs_source_set_monitoring_type(b,OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
    run(250);expect(.3,"Source replacement does not leave stale callbacks");
    obs_source_set_sync_offset(b,80000000);
    run(400);expect(.3,"Positive AV sync offsets retain continuous audio");
    obs_source_set_sync_offset(b,-80000000);
    run(500);expect(.3,"Negative AV sync offsets align rather than dropping every block");
    obs_source_set_async_unbuffered(b,true);obs_source_set_async_decoupled(b,true);
    run(350);expect(.3,"Unbuffered decoupled AV follows OBS's sync exception");
    obs_source_set_monitoring_type(a,OBS_MONITORING_TYPE_NONE);
    obs_source_set_monitoring_type(b,OBS_MONITORING_TYPE_NONE);
    run(250);expect(0,"No monitored sources sends continuous silence");
    check(left.packets.size()>20,"Silence keeps VBAN receive clocks running");
    dialog->grab().save(root+"/return-settings.png");
    // Source destruction while a capture producer is running.
    for(int i=0;i<20;++i){
        auto *source=obs_source_create("return_test_input","Churn",nullptr,nullptr);
        obs_source_inc_active(source);obs_source_set_monitoring_type(source,OBS_MONITORING_TYPE_MONITOR_ONLY);
        std::thread producer([&]{for(int n=0;n<10;++n){output(source,.001f,os_gettime_ns(),128);std::this_thread::sleep_for(std::chrono::milliseconds(1));}});
        for(int n=0;n<3;++n){obs_source_set_monitoring_type(source,n%2?OBS_MONITORING_TYPE_NONE:OBS_MONITORING_TYPE_MONITOR_ONLY);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        producer.join();obs_source_dec_active(source);obs_source_release(source);
    }
    dialog->reject();app.processEvents();
    obs_source_set_monitoring_type(a,OBS_MONITORING_TYPE_MONITOR_ONLY);
    obs_source_set_monitoring_type(b,OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
    run(250);expect(.3,"Active audio before frontend shutdown");
    frontend->on_event(OBS_FRONTEND_EVENT_EXIT);
    run(150);check(left.packets.empty() && right.packets.empty(),"Frontend exit detaches live sources and stops TX");
    obs_source_dec_active(a);obs_source_dec_active(b);obs_source_release(a);obs_source_release(b);
    obs_wait_for_destroy_queue();
    obs_shutdown();app.processEvents();obs_frontend_set_callbacks_internal(nullptr);
    std::cout<<"OBS monitor return passed: old-config migration, preexisting/new sources, filters/gain/mute, both monitor modes, off, 44.1-kHz mono, identical fan-out, invalid config rollback, independent enable, lifetime, active/removal changes and shutdown.\n";
    return 0;
 }catch(const std::exception &e){std::cerr<<"Monitor return failed: "<<e.what()<<"\n";return 1;}
}
