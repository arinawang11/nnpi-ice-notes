/* SPDX-License-Identifier: GPL-2.0-only */
#define NNPI_BRIDGE_MOCK
#include "../nnpi-net-bridge.cpp"
#include <deque>
#include <vector>
#include <memory>
#include <cstdlib>
using namespace bridge;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
struct Mock {
    std::deque<Frame> reads, receives;
    std::vector<Frame> transmitted, written;
    unsigned starts[LaneCount]{}, claims=0, releases=0, drains=0;
    unsigned busyLeft=0; bool claimOk=true, activateOk=true, lostTx=false, failTap=false;
    bool holdWrite=false, malformed=false, badIdentity=false, cancelled=false;
    bool shortWrite=false, oversizeRead=false, badTx=false;
    Identity id{};
    bool claim(Identity i) { ++claims; id=i; return claimOk; }
    bool activate() { return activateOk; }
    bool release(Identity) noexcept { CHECK(drains==1); ++releases; return true; }
    void deactivate() noexcept {}
    void start(Lane l, Op&) { ++starts[l]; }
    template<class T> void reply(Op& op,const T& r) { std::memcpy(op.out.data(),&r,sizeof(r));op.bytes=sizeof(r); }
    bool poll(Lane l,Op& op) {
        if (l==TapRead) {
            if(failTap){op.error=1167;return true;}
            if(oversizeRead){op.bytes=4097;return true;}
            if(reads.empty())return false;
            auto f=reads.front();reads.pop_front();op.bytes=f.size;std::memcpy(op.out.data(),f.data.data(),f.size);return true;
        }
        if(l==TapWrite){
            if(holdWrite)return false;
            Frame f{};f.size=op.inBytes;std::memcpy(f.data.data(),op.in.data(),f.size);written.push_back(f);op.bytes=shortWrite ? f.size-1 : f.size;return true;
        }
        if(l==NetTx){
            if(lostTx)return false;
            if(busyLeft){--busyLeft;op.error=Busy;return true;}
            nnpi_net_tx_request q{};std::memcpy(&q,op.in.data(),sizeof(q));
            CHECK(identity(q,id,NNPI_NET_DATA_VERSION));CHECK(q.length<=FrameBytes);
            for(size_t i=q.length;i<FrameBytes;++i)CHECK(q.frame[i]==0);
            Frame f{};f.size=q.length;std::memcpy(f.data.data(),q.frame,q.length);transmitted.push_back(f);
            nnpi_net_tx_response r{};bind(r,id,NNPI_NET_DATA_VERSION);r.accepted=badTx ? 0u : 1u;reply(op,r);return true;
        }
        nnpi_net_rx_batch_request q{};std::memcpy(&q,op.in.data(),sizeof(q));
        nnpi_net_rx_batch_response r{};bind(r,id,NNPI_NET_RX_VERSION);
        while(!receives.empty()&&r.count<q.max_frames){auto f=receives.front();receives.pop_front();auto& v=r.frames[r.count++];v.length=f.size;std::memcpy(v.frame,f.data.data(),f.size);}
        if(malformed)r.frames[3].frame[4095]=1;
        if(badIdentity)++r.epoch;
        reply(op,r);return true;
    }
    void cancelDrain(std::array<Op,LaneCount>&) noexcept {++drains;cancelled=true;}
};
Frame frame(unsigned type,unsigned seed=1,uint32_t size=64){Frame f{};f.size=size;for(uint32_t i=0;i<size;++i)f.data[i]=static_cast<uint8_t>(i+seed);if(size>=14){f.data[12]=static_cast<uint8_t>(type>>8);f.data[13]=static_cast<uint8_t>(type);}return f;}
void equal(const Frame&a,const Frame&b){CHECK(a.size==b.size);CHECK(std::memcmp(a.data.data(),b.data.data(),a.size)==0);}
void pump(Engine<Mock>&e,uint64_t start,unsigned n){for(unsigned i=0;i<n;++i){if(!e.tick(start+i))break;CHECK(e.toNet.count<=QueueSize);CHECK(e.toTap.count<=QueueSize);}}
int main(){unsigned cases=0;
    {Mock m;auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;auto arp=frame(0x806),ip=frame(0x800,7);m.reads={arp,ip};m.receives={ip,arp};CHECK(e.begin());pump(e,0,30);CHECK(m.transmitted.size()==2&&m.written.size()==2);equal(m.transmitted[0],arp);equal(m.transmitted[1],ip);equal(m.written[0],ip);equal(m.written[1],arp);e.stop();CHECK(m.drains==1&&m.releases==1);++cases;}
    {Mock m;m.busyLeft=7;m.reads.push_back(frame(0x806));auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,60);CHECK(m.transmitted.size()==1&&e.stats.busy==7&&e.stats.txAccepted==1);++cases;}
    {Mock m;m.lostTx=true;m.reads.push_back(frame(0x800));auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,20);CHECK(m.starts[NetTx]==1);CHECK(!e.tick(6000));CHECK(e.stats.uncertainTx==1);e.stop();CHECK(m.starts[NetTx]==1&&m.drains==1);++cases;}
    {Mock m;m.busyLeft=10000;m.holdWrite=true;for(unsigned i=0;i<200;++i){m.reads.push_back(frame(0x800,i));m.receives.push_back(frame(0x806,i));}auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,500);CHECK(e.toNet.count==QueueSize&&e.toTap.count==QueueSize);CHECK(!e.ops[TapRead].active&&!e.ops[NetRx].active);CHECK(m.starts[TapRead]==QueueSize);e.stop();CHECK(m.releases==1);++cases;}
    {Mock m;m.reads={frame(0,1,13),frame(0x800,1,4096)};m.receives={frame(0,1,0),frame(0x806,5,4096)};auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,40);CHECK(e.stats.shortDrops==2&&m.written.size()==1&&m.transmitted.size()==1);CHECK(m.written[0].size==4096&&m.transmitted[0].size==4096);++cases;}
    {Mock m;m.failTap=true;auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,5);CHECK(!e.running&&e.stats.errors==1);e.stop();CHECK(m.cancelled&&m.releases==1);++cases;}
    {Mock m;m.malformed=true;m.receives.push_back(frame(0x800));auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,5);CHECK(!e.running&&m.written.empty()&&e.toTap.count==0);++cases;}
    {Mock m;m.badIdentity=true;auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,5);CHECK(!e.running);++cases;}
    {Mock m;m.claimOk=false;auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(!e.begin());e.stop();CHECK(!m.releases&&m.drains==1);++cases;}
    {Mock m;m.activateOk=false;auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(!e.begin());e.stop();CHECK(m.releases==1&&m.drains==1);++cases;}
    {Mock m;for(unsigned i=0;i<300;++i){m.reads.push_back(frame(0x800,i));m.receives.push_back(frame(0x806,i));}auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,1500);CHECK(m.transmitted.size()==300&&m.written.size()==300);for(unsigned i=0;i<300;++i){equal(m.transmitted[i],frame(0x800,i));equal(m.written[i],frame(0x806,i));}++cases;}
    {Mock m;auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());CHECK(e.tick(0));e.stop();e.stop();CHECK(m.drains==1&&m.releases==1);CHECK(!e.tick(10));++cases;}
    {Mock m;m.lostTx=true;m.reads.push_back(frame(0x800));auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,4);CHECK(e.ops[NetTx].active);e.stop();CHECK(m.starts[NetTx]==1&&e.stats.uncertainTx==1&&m.drains==1&&m.releases==1);++cases;}
    {Mock m;m.badTx=true;m.reads.push_back(frame(0x800));auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,20);CHECK(!e.running&&e.stats.uncertainTx==1&&m.starts[NetTx]==1);++cases;}
    {Mock m;m.oversizeRead=true;auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,5);CHECK(!e.running&&m.transmitted.empty());++cases;}
    {Mock m;m.shortWrite=true;m.receives.push_back(frame(0x800));auto owner=std::make_unique<Engine<Mock>>(m,Identity{1,2,3});auto& e=*owner;CHECK(e.begin());pump(e,0,20);CHECK(!e.running&&m.starts[TapWrite]==1&&e.stats.tapWritten==0);++cases;}
    std::printf("BRIDGE_MOCK_TESTS_PASS cases=%u (no hardware, no TAP IOCTL simulation)\n",cases);return 0;
}
