// SPDX-License-Identifier: GPL-2.0-or-later
#include <winsock2.h>
#include <ws2tcpip.h>
#include "receiver.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>

using namespace vban;
static int checks = 0;
static void check(bool value, const char *what) {
    ++checks;
    if (!value) throw std::runtime_error(what);
}
static uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
static std::vector<uint8_t> wire(uint32_t seq = 0, uint8_t type = 1, uint8_t channels = 2,
                                 uint16_t frames = 240, const std::string &name = "MIC") {
    constexpr unsigned widths[]{1,2,3,4,4,8};
    std::vector<uint8_t> bytes(28 + size_t(frames)*channels*widths[type], 0);
    std::memcpy(bytes.data(), "VBAN", 4);
    bytes[4] = 3; bytes[5] = static_cast<uint8_t>(frames-1);
    bytes[6] = channels-1; bytes[7] = type;
    std::memcpy(bytes.data()+8, name.data(), std::min<size_t>(name.size(),16));
    for (unsigned i = 0; i < 4; ++i) bytes[24+i] = static_cast<uint8_t>(seq >> (i*8));
    if (type == 1) {
        for (size_t i = 28; i < bytes.size(); i += 2) { bytes[i] = 0; bytes[i+1] = 0x20; }
    }
    return bytes;
}
static Packet packet(uint32_t seq, uint16_t frames = 240, float sample = .25f) {
    Packet p;
    p.format = {48000,2,1}; p.sequence = seq; p.frames = frames; p.name = "MIC";
    p.samples.assign(size_t(frames)*2,sample);
    return p;
}
static void parser_tests() {
    Packet out;
    auto bytes = wire();
    check(decode(bytes.data(),bytes.size(),out) == ParseError::none, "PCM16 decode");
    check(out.frames == 240 && out.samples.size() == 480 && out.samples.front() == .25f, "PCM16 values");
    for (size_t n = 0; n < 28; ++n) check(decode(bytes.data(),n,out) == ParseError::truncated,"Short headers");
    auto invalid = bytes;
    invalid[0] = 0; check(decode(invalid.data(),invalid.size(),out) == ParseError::signature,"Signature");
    invalid = bytes; invalid[4] |= 0x20;
    check(decode(invalid.data(),invalid.size(),out) == ParseError::protocol,"Non-audio rejected");
    invalid = bytes; invalid[7] |= 0x10;
    check(decode(invalid.data(),invalid.size(),out) == ParseError::codec,"Compressed audio rejected");
    invalid = bytes; invalid[7] |= 8;
    check(decode(invalid.data(),invalid.size(),out) == ParseError::reserved,"Reserved bit rejected");
    invalid = bytes; invalid[4] = 31;
    check(decode(invalid.data(),invalid.size(),out) == ParseError::sample_rate,"Bad rate");
    invalid = bytes; invalid[6] = 8;
    check(decode(invalid.data(),invalid.size(),out) == ParseError::channels,"More than 8 channels");
    invalid = bytes; invalid[7] = 6;
    check(decode(invalid.data(),invalid.size(),out) == ParseError::sample_type,"12-bit rejected");
    invalid[7] = 7;
    check(decode(invalid.data(),invalid.size(),out) == ParseError::sample_type,"10-bit rejected");
    invalid = bytes; invalid.pop_back();
    check(decode(invalid.data(),invalid.size(),out) == ParseError::length,"Short payload");
    invalid = bytes; invalid.push_back(0);
    check(decode(invalid.data(),invalid.size(),out) == ParseError::length,"Extra payload");
    invalid = wire(0,1,8,256);
    check(decode(invalid.data(),invalid.size(),out) == ParseError::length,"Oversized datagram");

    bytes = wire(0xffffffffU,2,1,3,"1234567890ABCDEF");
    const uint8_t pcm24[]{0,0,0x80, 0xff,0xff,0x7f, 0xff,0xff,0xff};
    std::memcpy(bytes.data()+28,pcm24,9);
    check(decode(bytes.data(),bytes.size(),out) == ParseError::none,"PCM24 decode, full 16-byte name");
    check(out.sequence == UINT32_MAX && out.name.size() == 16,"Counter and name");
    check(out.samples[0] == -1 && out.samples[1] > .99999f && out.samples[2] < 0,"PCM24 sign extension");
    bytes = wire(0,0,1,3); bytes[28]=0;bytes[29]=128;bytes[30]=255;
    check(decode(bytes.data(),bytes.size(),out) == ParseError::none &&
        out.samples[0] == -1 && out.samples[1] == 0 && out.samples[2] == 127.f/128,"Unsigned PCM8");
    bytes = wire(0,3,1,2);bytes[31]=0x80;
    for (int i = 32; i < 35; ++i) bytes[i]=0xff;bytes[35]=0x7f;
    check(decode(bytes.data(),bytes.size(),out) == ParseError::none &&
        out.samples[0] == -1 && out.samples[1] > .9999f,"Signed PCM32");
    for (uint8_t type : {uint8_t(4),uint8_t(5)}) {
        bytes = wire(0,type,1,1);
        if (type == 4) { float sample=.375f;std::memcpy(bytes.data()+28,&sample,4); }
        else { double sample=.375;std::memcpy(bytes.data()+28,&sample,8); }
        check(decode(bytes.data(),bytes.size(),out) == ParseError::none && out.samples[0] == .375f,"Float decode");
        if (type == 4) { float sample=std::numeric_limits<float>::infinity();std::memcpy(bytes.data()+28,&sample,4); }
        else { double sample=std::numeric_limits<double>::quiet_NaN();std::memcpy(bytes.data()+28,&sample,8); }
        check(decode(bytes.data(),bytes.size(),out) == ParseError::nonfinite,"Reject NaN/Inf");
    }
    bytes = wire(0,1,7,1);
    check(decode(bytes.data(),bytes.size(),out) == ParseError::none &&
        out.samples.size() == 8 && out.samples[6] == .25f && out.samples[7] == 0,"Seven channel padding");
    for (uint8_t i = 0; i < sample_rates.size(); ++i) {
        bytes = wire(0,1,1,1);bytes[4] = i;
        check(decode(bytes.data(),bytes.size(),out) == ParseError::none && out.format.rate == sample_rates[i],"Sample rate table");
    }
    std::mt19937 rng(12345);
    for (int i = 0; i < 20000; ++i) {
        bytes.resize(rng()%1600);
        for (auto &b : bytes) b = static_cast<uint8_t>(rng());
        if (bytes.size() >= 4 && i%2) std::memcpy(bytes.data(),"VBAN",4);
        if (decode(bytes.data(),bytes.size(),out) == ParseError::none)
            check(out.samples.size() == size_t(out.frames)*out.format.output_channels(),"Fuzz output bounds");
    }
}
static void buffer_tests() {
    constexpr uint64_t t = 1000000000;
    StreamBuffer buffer;
    buffer.push(packet(0),t);
    buffer.push(packet(2),t+1000000);
    buffer.push(packet(1),t+2000000);
    buffer.push(packet(1),t+3000000);
    for (uint32_t i = 3; i < 8; ++i) buffer.push(packet(i),t+4000000+i*100000);
    check(!buffer.pull(t+29000000),"Startup jitter delay");
    auto block = buffer.pull(t+30000000);
    check(block && block->frames == 480 && block->timestamp == t+30000000,"Scheduled playout");
    check(buffer.counters().reordered == 1 && buffer.counters().duplicates == 1,"Reorder and duplicate counters");
    check(buffer.counters().lost == 0,"Reordered packets are not lost");
    auto next = buffer.pull(t+40000000);
    check(next && next->timestamp == block->timestamp+10000000,"Sample-derived timestamps");

    StreamBuffer loss;
    loss.push(packet(10,240,.5f),t);
    for (uint32_t i = 12; i < 17; ++i) loss.push(packet(i,240,.5f),t+1000000);
    auto missing = loss.pull(t+30000000);
    check(missing && loss.counters().lost == 1,"Missing packet counted");
    check(missing->samples[0] == .5f && missing->samples[600] == 0,"Missing packet concealed with silence");

    StreamBuffer wrap;
    for (uint32_t i = 0; i < 8; ++i) wrap.push(packet(UINT32_MAX-2+i),t+i*1000000);
    check(bool(wrap.pull(t+30000000)) && bool(wrap.pull(t+40000000)),"Counter wrap playout");
    check(wrap.counters().lost == 0 && wrap.counters().late == 0,"Counter wrap continuity");
    auto changed = packet(40);changed.format.rate=44100;
    wrap.push(changed,t+50000000);
    check(!wrap.pull(t+60000000),"Format change rebuffers");
    auto newer = wrap.pull(t+80000000);
    check(newer && newer->format.rate==44100 && newer->timestamp >= t+80000000,"Format change resets clock");
    wrap.reset();wrap.push(packet(0),t+81000000);
    auto reset = wrap.pull(t+111000000);
    check(reset && reset->timestamp > newer->timestamp,"Reset cannot move timestamps backward");
    StreamBuffer bounded;
    for (uint32_t i=0;i<10000;++i) bounded.push(packet(i),t);
    check(bounded.counters().overruns > 0,"Bounded buffer recovers from overload");
    check(!bounded.pull(t+1000000000),"Stalled host discards stale backlog");

    StreamBuffer restart;
    for (uint32_t i=10000;i<10008;++i) restart.push(packet(i),t);
    restart.pull(t+30000000);
    restart.push(packet(0),t+300000000);
    check(bool(restart.pull(t+330000000)),"Sender restart recovery");
    check(restart.counters().discontinuities > 0,"Restart recorded");
    check(frames_to_ns(48000ULL*86400*365,48000)==86400ULL*365*1000000000ULL,"Long-running timestamp arithmetic");
}

static void clock_drift_tests() {
    constexpr uint64_t origin = 1000000000ULL;
    for (const int ppm : {-500, 500}) {
        StreamBuffer buffer;
        uint64_t arrival = origin, last_timestamp = 0;
        const uint64_t period = static_cast<uint64_t>(5000000LL - int64_t(ppm) * 5);
        uint32_t sequence = 0;
        for (uint64_t now = origin; now < origin + 120000000000ULL; now += 1000000ULL) {
            while (arrival <= now) { buffer.push(packet(sequence++),arrival); arrival += period; }
            const auto block = buffer.pull(now);
            if (block) {
                check(block->timestamp > last_timestamp, "Drift simulation timestamps");
                last_timestamp = block->timestamp;
            }
        }
        check(buffer.counters().underruns == 0, "Clock drift does not underrun");
        check(buffer.counters().overruns == 0 && buffer.counters().lost == 0, "Clock drift stays bounded");
        check(buffer.counters().drift_corrections > 0, "Independent clock drift is corrected");
    }
}

class Sender {
public:
    SOCKET socket = INVALID_SOCKET;
    Sender() {
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(socket==INVALID_SOCKET) throw std::runtime_error("Sender socket");
    }
    ~Sender(){closesocket(socket);}
    void send(uint16_t port,const std::vector<uint8_t> &bytes) {
        sockaddr_in dest{};dest.sin_family=AF_INET;dest.sin_port=htons(port);
        InetPtonA(AF_INET,"127.0.0.1",&dest.sin_addr);
        if(sendto(socket,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0,
            reinterpret_cast<sockaddr*>(&dest),sizeof(dest))==SOCKET_ERROR) throw std::runtime_error("sendto failed");
    }
};
static uint16_t free_port() {
    Sender s;sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(s.socket,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0,"Reserve test port");
    int size=sizeof(a);getsockname(s.socket,reinterpret_cast<sockaddr*>(&a),&size);
    return ntohs(a.sin_port);
}
static void network_tests() {
    Receiver receiver(now_ns);
    Config cfg;cfg.sender_ip="127.0.0.1";cfg.port=free_port();
    for(size_t i=0;i<slot_count;++i) cfg.slots[i]={true,"Test","","S"+std::to_string(i)};
    std::string error;
    check(receiver.configure(cfg,error),"Receiver configuration");
    auto invalid=cfg;invalid.slots[1].stream_name="S0";
    check(!receiver.configure(invalid,error),"Duplicate slot definitions rejected");
    invalid=cfg;invalid.sender_ip="999.2.3.4";
    check(!receiver.configure(invalid,error),"Invalid IPv4 rejected");
    invalid=cfg;invalid.slots[0].stream_name=std::string(17,'x');
    check(!receiver.configure(invalid,error),"Name too long rejected");
    std::array<std::atomic<int>,9> counts{};
    std::vector<std::shared_ptr<Receiver::Consumer>> consumers;
    for(int i=0;i<9;++i) consumers.push_back(receiver.subscribe(i%8,[&counts,i](const AudioBlock &b){
        if(!b.samples.empty() && b.samples[0]>.1f) ++counts[i];
    }));
    Sender sender;
    for(uint32_t sequence=0;sequence<50;++sequence) {
        for(int i=0;i<8;++i) sender.send(cfg.port,wire(sequence,1,2,240,"S"+std::to_string(i)));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for(int i=0;i<9;++i) check(counts[i]>5,"Eight streams and duplicate consumers receive audio");
    check(std::abs(counts[0]-counts[8])<=1,"Second source does not steal audio");
    check(receiver.status(0).state==State::receiving,"Receiving is based on valid packets");
    auto before=receiver.status(0).counters.received;
    sender.send(cfg.port,wire(999,1,2,240,"UNMATCHED"));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(receiver.status(0).counters.received==before,"Unmatched name ignored");

    Config newcfg=cfg;newcfg.port=free_port();
    check(!receiver.configure(newcfg,error,[](const Config&,std::string &e){e="Simulated save failure";return false;}),"Save failure rejected");
    check(receiver.config().port==cfg.port,"Failed save preserves live receiver");

    Receiver competitor(now_ns);
    check(!competitor.configure(cfg,error),"Port collision reported");
    check(receiver.configure(newcfg,error),"Live port change");
    sender.send(newcfg.port,wire(0,1,2,240,"S0"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(receiver.status(0).counters.received==1,"New port receives after rebind");
    cfg=newcfg;cfg.common_ip=false;
    for(size_t i=0;i<8;++i) cfg.slots[i].sender_ip=i==0?"127.0.0.2":"127.0.0.1";
    check(receiver.configure(cfg,error),"Individual sender mode");
    sender.send(cfg.port,wire(1,1,2,240,"S0"));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(receiver.status(0).counters.received==0,"Mismatching sender ignored");
    cfg.slots[0].sender_ip="127.0.0.1";check(receiver.configure(cfg,error),"Sender reconfiguration");
    auto bad=wire(0,1,2,240,"S0");bad[7]=6;sender.send(cfg.port,bad);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(receiver.status(0).state==State::error && receiver.status(0).counters.unsupported==1,"Unsupported format status");
    sender.send(cfg.port,wire(1,1,2,240,"S0"));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(receiver.status(0).state==State::receiving,"Valid packet clears format error");
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    check(receiver.status(0).state==State::waiting,"Packet timeout");
    for(auto &consumer:consumers) receiver.unsubscribe(consumer);
    for(auto &slot:cfg.slots) slot.enabled=false;
    check(receiver.configure(cfg,error),"Disable receiver");
    check(receiver.status(0).state==State::disabled,"Disabled status");
    const auto start=now_ns();
    {Receiver idle(now_ns);}
    check(now_ns()-start<500000000,"Shutdown is bounded");
}
int main() {
    try {
        parser_tests();buffer_tests();clock_drift_tests();network_tests();
        std::cout << "Passed " << checks << " checks, including 20,000 malformed datagrams and UDP integration.\n";
        return 0;
    } catch(const std::exception &e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";return 1;
    }
}
