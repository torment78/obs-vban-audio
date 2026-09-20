// SPDX-License-Identifier: GPL-2.0-or-later
#include <winsock2.h>
#include <ws2tcpip.h>
#include "vban-transmitter.hpp"
#include "return-audio.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace vban;
static void check(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
int main() {
 try {
    std::array<uint8_t, max_datagram> bytes{};
    std::array<float, return_frames*2> samples{};
    samples[0] = -2; samples[1] = 2; samples[2] = 0.125f; samples[3] = std::nanf("");
    Packet packet;
    for (const auto rate : sample_rates) {
        auto length = encode_stereo24(bytes.data(), rate, "1234567890123456", UINT32_MAX, samples.data(), return_frames);
        check(decode(bytes.data(), length, packet) == ParseError::none, "TX packet decodes at every VBAN rate");
        check(packet.sequence == UINT32_MAX && packet.format.rate == rate && packet.format.type == 2 &&
              packet.format.channels == 2 && packet.name == "1234567890123456", "TX header fields");
        check(packet.samples[0] == -1 && packet.samples[1] < 1 && packet.samples[1] > .999f &&
              packet.samples[2] == .125f && packet.samples[3] == 0, "PCM24 clamp and nonfinite sanitization");
    }
    check(!encode_stereo24(bytes.data(),48000,"seventeen-chars!!x",0,samples.data(),128),"Reject overlong name");
    check(!encode_stereo24(bytes.data(),48001,"VALID",0,samples.data(),128),"Reject unsupported rate");
    check(!encode_stereo24(bytes.data(),48000,"VALID",0,samples.data(),240),"Respect VBAN datagram size");
    uint32_t sequence = UINT32_MAX;
    encode_stereo24(bytes.data(),48000,"VALID",sequence++,samples.data(),128);
    const auto length = encode_stereo24(bytes.data(),48000,"VALID",sequence++,samples.data(),128);
    check(decode(bytes.data(),length,packet)==ParseError::none && packet.sequence==0,"Frame counter rollover");
    AudioRing<int,4> small;
    for(int i=0;i<4;++i) { auto *p=small.write_slot();check(p,"Ring slot");*p=i;small.publish(); }
    check(!small.write_slot(),"Full ring drops without blocking");
    for(int i=0;i<4;++i){check(small.front() && *small.front()==i,"Ring order");small.pop();}
    for(int i=4;i<20;++i){*small.write_slot()=i;small.publish();check(*small.front()==i,"Ring wrap");small.pop();}
    AudioRing<uint64_t,256> concurrent;
    std::thread producer([&] { for(uint64_t i=0;i<200000;++i){uint64_t *p;while(!(p=concurrent.write_slot())) std::this_thread::yield();*p=i;concurrent.publish();} });
    for(uint64_t i=0;i<200000;++i){const uint64_t *p;while(!(p=concurrent.front()))std::this_thread::yield();check(*p==i,"Concurrent ring ordering");concurrent.pop();}
    producer.join();
    StereoTimeline a,b; a.resize(2048);b.resize(2048);
    std::array<float,512> one{},two{},mix{};
    one.fill(.1f);two.fill(.2f);
    a.put(1040,one.data(),100,1,1000);
    a.put(1040,one.data(),100,1,1000); // duplicate must not double.
    b.put(1060,two.data(),100,.5f,1000);
    a.mix(1000,mix.data(),256);b.mix(1000,mix.data(),256);
    check(mix[0]==0 && mix[80]==.1f && std::fabs(mix[120]-.2f)<1e-6f &&
          mix[280]==.1f && mix[320]==0,"Aligned overlap, gaps, gain and duplicate suppression");
    mix.fill(0);a.mix(1000,mix.data(),256);b.mix(1000,mix.data(),256);
    check(std::all_of(mix.begin(),mix.end(),[](float v){return v==0;}),"Consumed frames do not repeat");
    check(a.put(900,one.data(),100,1,1000)==100,"Count late audio separately from network loss"); a.put(4000,one.data(),100,1,1000);
    a.mix(1000,mix.data(),256); check(mix[0]==0,"Late/far-future frames cannot corrupt timeline");
    MonitorClock clock;
    const uint64_t now=100000000000ULL;
    check(clock.position(now,now+5000000,256,48000)==4800000,"Direct clock preserves timestamps despite arrival delay");
    check(clock.position(now+frames_to_ns(256,48000),now+15000000,256,48000)==4800256,"Timestamp rounding is smoothed");
    clock.reset();
    check(clock.position(0,now,256,48000)==4800000,"Foreign media epoch anchored to monotonic time");
    check(clock.position(frames_to_ns(256,48000),now+20000000,256,48000)==4800256,"Foreign clock ignores ordinary callback jitter");
    // Reproduce slowly drifting source clocks: adjacent PCM blocks must remain contiguous.
    for (const int ppm : {-500, 500}) {
        MonitorClock drifting;
        int64_t end = 0;
        uint64_t gaps = 0, overlaps = 0;
        for (uint64_t i = 0; i < 2000; ++i) {
            const auto stamp = now + static_cast<uint64_t>(
                static_cast<long double>(i * 256) * (1000000000.0L + ppm * 1000.0L) / 48000.0L);
            const auto start = drifting.position(stamp, stamp + 5000000, 256, 48000);
            if (i && start > end) gaps += start - end;
            if (i && start < end) overlaps += end - start;
            end = start + drifting.frames();
        }
        std::cout << "Clock " << ppm << " ppm: gap frames=" << gaps << ", overlap frames=" << overlaps << "\n";
        check(gaps == 0 && overlaps == 0, "Slow source-clock drift must not cut holes or overlaps in return audio");
    }
    // Two minutes of continuous 1 kHz audio in both clock directions and at multiple rates.
    for (uint32_t rate : {44100u, 48000u, 96000u, 192000u}) for (int ppm : {-1000, 1000}) {
        MonitorClock drift; StereoRetimer retimer;
        std::array<float, 512> wave{};
        int64_t end = 0; uint64_t corrections = 0;
        float previous = 0; bool heard = false; double largest_step = 0;
        for (uint64_t total = 0; total < uint64_t(rate)*120; total += 256) {
            for (size_t i = 0; i < 256; ++i)
                wave[i*2] = wave[i*2+1] = .2f * std::sin(float((total+i)*6.283185307179586*1000.0/rate));
            const auto stamp = now + static_cast<uint64_t>(
                static_cast<long double>(total) * (1000000000.0L + ppm*1000.0L) / rate);
            const auto start = drift.position(stamp, stamp+5000000, 256, rate);
            check(!total || start == end, "Drifting audio keeps contiguous output sample positions");
            check(!drift.discontinuity(), "Normal clock drift is not a discontinuity");
            corrections += drift.frames() != 256;
            const auto *pcm = retimer.process(wave.data(),256,drift.frames());
            for(size_t i=0;i<drift.frames();++i){
                check(std::isfinite(pcm[i*2]) && pcm[i*2]==pcm[i*2+1],"Retimed stereo remains finite and matched");
                if(heard)largest_step=std::max(largest_step,double(std::fabs(pcm[i*2]-previous)));
                previous=pcm[i*2];heard=true;
            }
            end=start+drift.frames();
            check(std::llabs(end-(ns_to_frames(stamp,rate)+256))<=3,"Clock correction bounds long-term drift");
        }
        check(corrections>100 && largest_step<.04,"Clock drift produces no sudden waveform steps");
    }
    // Foreign media clocks must follow real arrival rate, without drifting outside the return buffer.
    for(int ppm : {-1000,1000}){
        MonitorClock foreign;
        int64_t previous_end=0;int64_t max_skew=0;
        for(uint64_t i=0;i<24000;++i){
            const auto stamp=frames_to_ns(i*256,48000);
            const auto arrival=now+static_cast<uint64_t>(
                static_cast<long double>(i*256)*(1000000000.0L+ppm*1000.0L)/48000.0L)+i%7*250000ULL;
            const auto start=foreign.position(stamp,arrival,256,48000);
            check(!i || start==previous_end,"Foreign drift keeps PCM positions continuous");
            previous_end=start+foreign.frames();
            max_skew=std::max(max_skew,std::llabs(start-ns_to_frames(arrival,48000)));
        }
        std::cout<<"Foreign clock "<<ppm<<" ppm: maximum skew="<<double(max_skew)/48.0<<" ms\n";
        check(max_skew<48000/50,"Foreign source-clock drift stays inside the minimum return buffer");
    }
    StereoRetimer pass;
    check(pass.process(one.data(),100,100)==one.data(),"Uncorrected PCM passes through exactly");
    // Callback latency: demonstrate a real deadline miss at 30 ms and cover the 60 ms default.
    auto delayed_audio=[](int buffer_ms){
        StereoTimeline line; line.resize(48000);
        std::array<float,960> input{};input.fill(.25f);
        size_t next=0;uint64_t late=0,missing=0;
        for(int ms=100;ms<2100;++ms){
            const int64_t cursor=int64_t(ms-buffer_ms)*48;
            while(int(next)*10+145<=ms){
                late+=line.put(int64_t(next)*480+4800,input.data(),480,1,cursor);++next;
            }
            std::array<float,96> out{};line.mix(cursor,out.data(),48);
            if(ms>=300)for(float sample:out)missing+=sample!=.25f;
        }
        return std::make_pair(late,missing);
    };
    check(delayed_audio(30).first>0 && delayed_audio(30).second>0,"30 ms deadline reproduces delayed-callback gaps");
    check(delayed_audio(60)==std::make_pair(uint64_t(0),uint64_t(0)),"60 ms absorbs 45 ms callback delay without holes");
    // Queue capacity exceeds a 200 ms worker stall at 48 kHz with 128-frame callbacks.
    auto stall_queue=std::make_unique<AudioRing<MonitorBlock,capture_blocks>>();
    for(size_t i=0;i<75;++i){check(stall_queue->write_slot(),"Capture reserve handles 200 ms stall");stall_queue->publish();}
    check(stall_queue->size()==75,"Queue occupancy telemetry");
    Transmitter tx;
    SOCKET sockets[2]{INVALID_SOCKET,INVALID_SOCKET};
    ReturnConfigs cfg=default_returns();
    for(size_t i=0;i<2;++i){
        sockets[i]=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in address{};address.sin_family=AF_INET;
        InetPtonA(AF_INET,i?"127.0.0.2":"127.0.0.1",&address.sin_addr);
        check(bind(sockets[i],reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Bind return destinations");
        int size=sizeof(address);getsockname(sockets[i],reinterpret_cast<sockaddr*>(&address),&size);
        DWORD timeout=500;setsockopt(sockets[i],SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        cfg[i]={true,i?"127.0.0.2":"127.0.0.1",ntohs(address.sin_port),i?"SECOND":"FIRST"};
    }
    std::string error;
    auto ready=tx.prepare(cfg,error);check(bool(ready),"Prepare two independent destinations");tx.activate(ready);
    check(tx.status(0).state==ReturnState::ready,"Ready before first packet");
    std::array<uint8_t,max_datagram> first{},second{};
    sockaddr_in peers[2]{};
    auto receive=[&](size_t i,auto &buffer){
        int size=sizeof(peers[i]);
        const int got=recvfrom(sockets[i],reinterpret_cast<char*>(buffer.data()),static_cast<int>(buffer.size()),0,
            reinterpret_cast<sockaddr*>(&peers[i]),&size);
        check(got>0,"Receive actual UDP return");return got;
    };
    tx.send(samples.data(),128,48000);
    check(tx.status(0).clipped_samples==2 && tx.status(0).nonfinite_samples==1,
          "Clipping and invalid PCM are diagnosed independently of socket loss");
    const auto n1=receive(0,first),n2=receive(1,second);
    for(size_t i=0;i<2;++i){
        const auto s=tx.status(i);char ip[INET_ADDRSTRLEN]{};
        InetNtopA(AF_INET,&peers[i].sin_addr,ip,sizeof(ip));
        check(s.source_ip==ip && s.source_port==ntohs(peers[i].sin_port) &&
              s.destination_ip==cfg[i].destination_ip && s.destination_port==cfg[i].destination_port,
              "Reported automatic source and destination match actual packets");
    }
    check(n1==n2 && std::equal(first.begin()+header_size,first.begin()+n1,second.begin()+header_size),"Both destinations carry identical PCM");
    cfg[0].enabled=false;ready=tx.prepare(cfg,error);tx.activate(ready);
    tx.send(samples.data(),128,48000);receive(1,second);
    cfg[0].enabled=true;ready=tx.prepare(cfg,error);tx.activate(ready);
    tx.send(samples.data(),128,48000);receive(0,first);receive(1,second);
    Packet p1,p2;decode(first.data(),n1,p1);decode(second.data(),n2,p2);
    check(p1.sequence==1 && p2.sequence==2,"Independent counters when one return is disabled");
    const auto valid=cfg;
    cfg[0].stream_name=std::string(17,'A');check(!tx.prepare(cfg,error),"Invalid names are rejected, never truncated");
    cfg=valid;cfg[0].destination_ip="bad";check(!tx.prepare(cfg,error),"Reject invalid destination");
    cfg=valid;cfg[0].destination_port=0;check(!tx.prepare(cfg,error),"Reject port zero");
    // An unreachable UDP peer must not stall or prevent sending to the other destination.
    cfg=valid;cfg[0].destination_ip="192.0.2.1";ready=tx.prepare(cfg,error);check(bool(ready),"Prepare unavailable destination");
    tx.activate(ready);tx.send(samples.data(),128,48000);receive(1,second);
    check(tx.status(1).state==ReturnState::sending,"Independent healthy destination remains sending");
    ready=tx.prepare(valid,error,"127.0.0.1");check(bool(ready),"Select an explicit active IPv4 adapter");tx.activate(ready);
    tx.send(samples.data(),128,48000);receive(0,first);receive(1,second);
    for(size_t i=0;i<2;++i){
        char ip[INET_ADDRSTRLEN]{};InetNtopA(AF_INET,&peers[i].sin_addr,ip,sizeof(ip));
        check(std::string(ip)=="127.0.0.1" && tx.status(i).source_ip==ip,"Both destinations use the selected sender IPv4");
    }
    check(!tx.prepare(valid,error,"192.0.2.123"),"Unavailable address cannot silently fall back");
    check(!tx.prepare(valid,error,"bad"),"Malformed local address rejected");
    check(!tx.prepare(valid,error,"0.0.0.0"),"Wildcard cannot masquerade as explicit selection");
    tx.send(samples.data(),128,48000);receive(0,first);receive(1,second);
    check(tx.status(0).source_ip=="127.0.0.1","Rejected selection preserves active sender");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(tx.status(0).state==ReturnState::stalled,"Sending cannot remain stale when worker stops");
    tx.send(samples.data(),128,48000);receive(0,first);receive(1,second);
    check(tx.status(0).state==ReturnState::sending && tx.status(0).send_gaps>=1 &&
          tx.status(0).max_send_gap_ms>=1000,"Report sender scheduling gaps and recover live status");
    for(auto s:sockets)closesocket(s);
    std::cout<<"Return core passed: PCM24 protocol, rollover, concurrent ring, timeline alignment, two actual UDP destinations, independent counters and invalid settings.\n";
    return 0;
 } catch(const std::exception &e) { std::cerr<<"Return test failed: "<<e.what()<<"\n";return 1; }
}
