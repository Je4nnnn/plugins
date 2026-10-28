#include "Core.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace beatweaver {
namespace {
constexpr const char* names[kParameterCount] = {
    "Time mix", "Volume mix", "Attack", "Release", "Tension", "Time slot", "Volume slot",
    "Trigger sync", "Position sync", "Pattern length", "Link slots", "Key held", "One shot",
    "Hold", "Hold mode", "Scratch", "Click fade", "Remove DC", "MIDI", "Bypass", "Output"
};
constexpr const char* timeNames[kSlotCount] = {
    "Straight", "Half Speed", "Reverse", "Double Speed", "Tape Stop", "Tape Start",
    "Repeat 2", "Repeat 4", "Repeat 8", "Repeat 16", "Reverse 2", "Reverse 4",
    "Backspin", "Vinyl Nudge", "Scratch Up", "Scratch Down", "Saw Scratch", "Triplet Repeat",
    "Half Then Straight", "Straight Then Half", "Reverse Tail", "Reverse Head", "Ping Pong Time", "Stutter Roll",
    "Quarter Speed", "Three Quarter", "Speed Ramp", "Slow Ramp", "Skip Ahead", "Shuffle Time",
    "Turntable", "Broken Tape", "Freeze", "Echo Phrase", "Reverse Triplet", "Tape Drift"
};
constexpr const char* volumeNames[kSlotCount] = {
    "Open", "Gate 2", "Gate 4", "Gate 8", "Gate 16", "Sidechain", "Pump Soft", "Pump Deep",
    "Fade In", "Fade Out", "Triangle", "Tremolo", "Triplet Gate", "Offbeat", "Pulse Narrow", "Pulse Wide",
    "Ramp 2", "Ramp 4", "Ramp 8", "Swell", "Duck 2", "Duck 4", "Chop Swing", "Broken Gate",
    "Accent", "Decay", "Double Duck", "Tremolo Fast", "Pulse Rise", "Pulse Fall", "Stair Up", "Stair Down",
    "Heartbeat", "Syncopation", "Silence", "Soft Open"
};

void append(Envelope& e, float x, float y, int curve = 0, float tension = 0) noexcept {
    if (e.count < kMaxPoints) e.points[e.count++] = {x,y,tension,curve};
}
void nameEnvelope(Envelope& e, const char* name) noexcept {
    std::snprintf(e.name, sizeof(e.name), "%s", name);
}
void ramp(Envelope& e, float from, float to) noexcept {
    e.count = 0; append(e,0,from); append(e,1,to);
}
void repeat(Envelope& e, int divisions, bool reverse) noexcept {
    e.count = 0;
    if (!reverse) {
        for (int i=0;i<divisions;++i) append(e,float(i)/divisions, .5f*i/divisions,1);
        append(e,1,.5f*(divisions-1)/divisions,1);
    } else {
        for (int i=0;i<divisions;++i) {
            append(e,float(i)/divisions,.5f*i/divisions);
            append(e,float(i+1)/divisions-.00001f,.5f*(i+2)/divisions);
        }
        append(e,1,.5f,0);
    }
}
void wave(Envelope& e, int cycles, int kind, float low = 0, float high = 1) noexcept {
    e.count = 0;
    const int steps = std::min(60,cycles*8);
    constexpr float pi = 3.14159265358979323846f;
    for (int i=0;i<=steps;++i) {
        const float x=float(i)/steps, phase=x*cycles-std::floor(x*cycles);
        float y=0;
        if(kind==0) y=.5f-.5f*std::cos(x*cycles*2*pi);
        else if(kind==1) y=1-std::fabs(phase*2-1);
        else y=phase;
        append(e,x,low+(high-low)*y,kind==0?2:0);
    }
}
void gate(Envelope& e, int divisions, float duty, float low = 0) noexcept {
    e.count=0;
    for(int i=0;i<divisions&&e.count<kMaxPoints-3;++i) {
        append(e,float(i)/divisions,1,1);
        append(e,(i+duty)/divisions,low,1);
    }
    append(e,1,low,1);
}
void pump(Envelope& e, int divisions, float low, float curve) noexcept {
    e.count=0;
    for(int i=0;i<divisions&&e.count<kMaxPoints-4;++i) {
        const float x=float(i)/divisions;
        append(e,x,low,3,curve);
        append(e,x+.65f/divisions,1,2);
        append(e,float(i+1)/divisions-.00001f,1,1);
    }
    append(e,1,1,0);
}
}

float clamp01(float value) noexcept { return std::isfinite(value)?std::max(0.0f,std::min(1.0f,value)):0.0f; }
int parameterSlot(float value) noexcept { return int(std::lround(clamp01(value)*(kSlotCount-1))); }
float slotParameter(int slot) noexcept { return float(std::max(0,std::min(kSlotCount-1,slot)))/(kSlotCount-1); }
double syncBeats(float value) noexcept {
    constexpr double values[]{0,.0625,.125,.25,.5,1,2,4,8};
    return values[int(std::lround(clamp01(value)*8))];
}
double lengthBeats(float value) noexcept {
    constexpr double values[]{.25,.5,1,2,4,8,16};
    return values[int(std::lround(clamp01(value)*6))];
}
float defaultParameter(Param p) noexcept {
    switch(p) {
    case Param::TimeMix: case Param::VolumeMix: case Param::RemoveDC: case Param::MidiEnable: return 1;
    case Param::Attack: return .02f;
    case Param::Release: return .08f;
    case Param::Tension: case Param::HoldMode: case Param::Output: return .5f;
    case Param::TriggerSync: return 5.0f/8;
    case Param::PositionSync: return 7.0f/8;
    case Param::Length: return 4.0f/6;
    case Param::Smooth: return .02f;
    default: return 0;
    }
}
const char* parameterName(Param p) noexcept {
    const int i=static_cast<int>(p); return i>=0&&i<kParameterCount?names[i]:"Unknown";
}
void parameterText(Param p, float v, char* text, int capacity) noexcept {
    if(!text||capacity<=0)return;
    v=clamp01(v);
    switch(p) {
    case Param::TimeSlot: case Param::VolumeSlot: std::snprintf(text,capacity,"%d",parameterSlot(v)+1);break;
    case Param::TriggerSync: case Param::PositionSync:
        if(syncBeats(v)==0)std::snprintf(text,capacity,"Free");
        else std::snprintf(text,capacity,"%g beats",syncBeats(v));break;
    case Param::Length:std::snprintf(text,capacity,"%g beats",lengthBeats(v));break;
    case Param::HoldMode:std::snprintf(text,capacity,"%s",v<.25f?"Off":v<.75f?"Last":"Next");break;
    case Param::Link: case Param::KeyHeld: case Param::OneShot: case Param::Hold:
    case Param::RemoveDC: case Param::MidiEnable: case Param::Bypass:
        std::snprintf(text,capacity,"%s",v>=.5f?"On":"Off");break;
    case Param::Attack:std::snprintf(text,capacity,"%.1f ms",v*500);break;
    case Param::Release:std::snprintf(text,capacity,"%.1f ms",v*1000);break;
    case Param::Smooth:std::snprintf(text,capacity,"%.1f ms",.1f+v*20);break;
    case Param::Tension:std::snprintf(text,capacity,"%+.0f %%",(v-.5f)*200);break;
    case Param::Output:
        if(v<=0)std::snprintf(text,capacity,"-inf dB");
        else std::snprintf(text,capacity,"%+.1f dB",20*std::log10(v*2));break;
    case Param::Scratch:std::snprintf(text,capacity,"%.0f %%",v*200);break;
    default:std::snprintf(text,capacity,"%.0f %%",v*100);break;
    }
}
Envelope defaultEnvelope(bool volume) noexcept {
    Envelope e; e.points[0]={0,volume?1.0f:0.0f,0,0};e.points[1]={1,e.points[0].y,0,0};
    nameEnvelope(e,volume?"Open":"Straight");return e;
}
Envelope makePreset(bool volume,int index) noexcept {
    index=std::max(0,std::min(kSlotCount-1,index)); Envelope e=defaultEnvelope(volume);
    if(!volume) {
        switch(index) {
        case 1:ramp(e,0,.25f);break;
        case 2:ramp(e,0,1);break;
        case 3:ramp(e,.5f,0);break;
        case 4:e.count=0;for(int i=0;i<=16;++i){float x=i/16.0f;append(e,x,x*x*.25f,2);}break;
        case 5:e.count=0;for(int i=0;i<=16;++i){float x=i/16.0f;append(e,x,.5f*x-.25f*x*x,2);}break;
        case 6:case 7:case 8:case 9:repeat(e,1<<(index-5),false);break;
        case 10:case 11:repeat(e,1<<(index-9),true);break;
        case 12:e.count=0;append(e,0,0);append(e,.25f,0);append(e,.55f,.55f,2);append(e,1,.65f,2);break;
        case 13:wave(e,2,0,0,.025f);break;
        case 14:e.count=0;append(e,0,.25f,2);append(e,.3f,0,2);append(e,.6f,.4f,2);append(e,1,0,2);break;
        case 15:e.count=0;append(e,0,0,2);append(e,.4f,.45f,2);append(e,.7f,.1f,2);append(e,1,.5f,2);break;
        case 16:wave(e,4,1,0,.2f);break;
        case 17:repeat(e,3,false);break;
        case 18:e.count=0;append(e,0,0);append(e,.5f,.125f);append(e,1,.125f);break;
        case 19:e.count=0;append(e,0,0);append(e,.5f,0);append(e,1,.125f);break;
        case 20:e.count=0;append(e,0,0);append(e,.5f,0);append(e,1,.5f);break;
        case 21:e.count=0;append(e,0,0);append(e,.5f,.5f);append(e,1,.5f);break;
        case 22:wave(e,2,1,0,.25f);break;
        case 23:e.count=0;append(e,0,0,1);append(e,.5f,0,1);append(e,.625f,.0625f,1);append(e,.75f,.125f,1);append(e,.8125f,.15625f,1);append(e,.875f,.1875f,1);append(e,.9375f,.21875f,1);append(e,1,.25f,1);break;
        case 24:ramp(e,0,.375f);break;
        case 25:ramp(e,0,.125f);break;
        case 26:e.count=0;for(int i=0;i<=16;++i){float x=i/16.0f;append(e,x,.5f*(1-x*x),2);}break;
        case 27:e.count=0;for(int i=0;i<=16;++i){float x=i/16.0f;append(e,x,.25f*x*x,2);}break;
        case 28:e.count=0;append(e,0,.5f,1);append(e,.25f,.375f,1);append(e,.5f,.25f,1);append(e,.75f,.125f,1);append(e,1,0,1);break;
        case 29:e.count=0;for(int i=0;i<=8;++i)append(e,i/8.0f,(i%2)?.025f:0,2);break;
        case 30:wave(e,1,0,0,.5f);break;
        case 31:e.count=0;append(e,0,0);append(e,.2f,.1f);append(e,.21f,0);append(e,.45f,.25f);append(e,.46f,.05f);append(e,.7f,.35f);append(e,.71f,.1f);append(e,1,.4f);break;
        case 32:ramp(e,0,.5f);break;
        case 33:ramp(e,.25f,.25f);break;
        case 34:repeat(e,3,true);break;
        case 35:wave(e,1,0,.01f,.025f);break;
        default:break;
        }
        nameEnvelope(e,timeNames[index]);
    } else {
        switch(index) {
        case 1:case 2:case 3:case 4:gate(e,1<<index,.5f);break;
        case 5:pump(e,4,0,.5f);break;
        case 6:pump(e,4,.4f,.2f);break;
        case 7:pump(e,4,0,.85f);break;
        case 8:ramp(e,0,1);break;
        case 9:ramp(e,1,0);break;
        case 10:wave(e,1,1);break;
        case 11:wave(e,4,0,.25f,1);break;
        case 12:gate(e,6,.5f);break;
        case 13:e.count=0;for(int i=0;i<4;++i){append(e,i/4.0f,0,1);append(e,(i+.5f)/4,1,1);}append(e,1,1,1);break;
        case 14:gate(e,4,.2f);break;
        case 15:gate(e,4,.8f);break;
        case 16:case 17:case 18:pump(e,1<<(index-15),0,0);break;
        case 19:e.count=0;append(e,0,0,2);append(e,.8f,1,2);append(e,1,0,2);break;
        case 20:pump(e,2,0,.6f);break;
        case 21:pump(e,4,.15f,.6f);break;
        case 22:e.count=0;for(int i=0;i<4;++i){append(e,i/4.0f,1,1);append(e,(i+.65f)/4,0,1);}append(e,1,0,1);break;
        case 23:e.count=0;append(e,0,1,1);append(e,.125f,0,1);append(e,.375f,1,1);append(e,.5f,0,1);append(e,.5625f,1,1);append(e,.625f,0,1);append(e,.875f,1,1);append(e,1,0,1);break;
        case 24:wave(e,4,0,.5f,1);break;
        case 25:e.count=0;append(e,0,1,3,.6f);append(e,1,0);break;
        case 26:pump(e,2,.15f,.3f);break;
        case 27:wave(e,8,0,.2f,1);break;
        case 28:e.count=0;for(int i=0;i<4;++i){append(e,i/4.0f,float(i+1)/4,1);append(e,(i+.5f)/4,0,1);}append(e,1,0);break;
        case 29:e.count=0;for(int i=0;i<4;++i){append(e,i/4.0f,1-float(i)/4,1);append(e,(i+.5f)/4,0,1);}append(e,1,0);break;
        case 30:e.count=0;for(int i=0;i<8;++i)append(e,i/8.0f,float(i+1)/8,1);append(e,1,1);break;
        case 31:e.count=0;for(int i=0;i<8;++i)append(e,i/8.0f,1-float(i)/8,1);append(e,1,0);break;
        case 32:e.count=0;append(e,0,0,2);append(e,.1f,1,2);append(e,.2f,0,2);append(e,.3f,.6f,2);append(e,.45f,0,1);append(e,1,0);break;
        case 33:e.count=0;append(e,0,1,1);append(e,.1875f,0,1);append(e,.375f,1,1);append(e,.5f,0,1);append(e,.75f,1,1);append(e,.875f,0,1);append(e,1,0);break;
        case 34:ramp(e,0,0);break;
        case 35:ramp(e,.75f,.75f);break;
        default:break;
        }
        nameEnvelope(e,volumeNames[index]);
    }
    return e;
}

float evaluateEnvelope(const Envelope& e,float x) noexcept {
    if(e.count<2)return 0;
    x=clamp01(x);
    const int n=std::max(2,std::min(kMaxPoints,e.count));
    if(x<=e.points[0].x)return clamp01(e.points[0].y);
    for(int i=1;i<n;++i) {
        const Point& a=e.points[i-1];const Point& b=e.points[i];
        if(x<=b.x||i==n-1) {
            if(b.x-a.x<=.0000001f)return clamp01(b.y);
            float t=clamp01((x-a.x)/(b.x-a.x));
            switch(a.curve) {
            case 1:t=t>=1.0f?1.0f:0.0f;break;
            case 2:t=t*t*(3-2*t);break;
            case 3: {
                const float tension=std::max(-1.0f,std::min(1.0f,a.tension));
                const float power=std::pow(8.0f,std::fabs(tension));
                t=tension>=0?std::pow(t,power):1-std::pow(1-t,power);break;
            }
            case 4: {
                const float power=std::pow(8.0f,std::fabs(a.tension));
                t=t<.5f?.5f*std::pow(t*2,power):1-.5f*std::pow((1-t)*2,power);
                if(a.tension<0)t=.5f-.5f*std::cos(t*3.14159265358979323846f);break;
            }
            case 5: {
                const float split=.5f+.4f*std::max(-1.0f,std::min(1.0f,a.tension));
                t=t<split?.5f*std::pow(t/split,2.0f):1-.5f*std::pow((1-t)/(1-split),2.0f);break;
            }
            case 6: {
                const float bias=.5f+.4f*std::max(-1.0f,std::min(1.0f,a.tension));
                const float shaped=t*t*(3-2*t);
                t=shaped/(shaped+(1-shaped)*std::max(.05f,bias*2));break;
            }
            case 7:case 8: {
                const float steps=2.0f+std::floor(std::fabs(a.tension)*14);
                const float scaled=t*steps,base=std::floor(scaled),fraction=scaled-base;
                t=(base+(a.curve==8?fraction*fraction*(3-2*fraction):0))/steps;break;
            }
            case 9: {
                const float cycles=1+std::floor(std::fabs(a.tension)*7);
                const float p=t*cycles-std::floor(t*cycles);t=t>=1.0f?1.0f:(p<.5f?0.0f:1.0f);break;
            }
            case 10: {
                const float cycles=1+std::floor(std::fabs(a.tension)*7);
                t=.5f-.5f*std::cos(t*cycles*6.28318530717958647692f);break;
            }
            case 11:t=std::sin(t*1.57079632679489661923f);break;
            default:break;
            }
            return clamp01(a.y+(b.y-a.y)*t);
        }
    }
    return clamp01(e.points[n-1].y);
}

Model::Model() {
    for(int i=0;i<kParameterCount;++i)params_[i].store(defaultParameter(static_cast<Param>(i)),std::memory_order_relaxed);
    resetPresets();
}
float Model::getParam(Param p) const noexcept {
    const int i=static_cast<int>(p);return i>=0&&i<kParameterCount?params_[i].load(std::memory_order_relaxed):0;
}
void Model::setParam(Param p,float value) noexcept {
    const int i=static_cast<int>(p);if(i>=0&&i<kParameterCount)params_[i].store(clamp01(value),std::memory_order_relaxed);
}
uint32_t Model::envelopeRevision(bool volume,int slot) const noexcept {
    slot=std::max(0,std::min(kSlotCount-1,slot));
    return envelopes_[slot+(volume?kSlotCount:0)].revision.load(std::memory_order_acquire);
}
bool Model::readEnvelope(bool volume,int slot,Envelope& out,uint32_t* revision) const noexcept {
    slot=std::max(0,std::min(kSlotCount-1,slot));const auto& source=envelopes_[slot+(volume?kSlotCount:0)];
    for(int attempt=0;attempt<3;++attempt) {
        const uint32_t before=source.revision.load(std::memory_order_acquire);if(before&1u)continue;
        Envelope copy;copy.count=source.count.load(std::memory_order_relaxed);
        for(int i=0;i<kMaxPoints;++i) {
            copy.points[i].x=source.points[i].x.load(std::memory_order_relaxed);
            copy.points[i].y=source.points[i].y.load(std::memory_order_relaxed);
            copy.points[i].tension=source.points[i].tension.load(std::memory_order_relaxed);
            copy.points[i].curve=source.points[i].curve.load(std::memory_order_relaxed);
        }
        for(int i=0;i<kNameLength;++i)copy.name[i]=source.name[i].load(std::memory_order_relaxed);
        // Atomic point fields make retries race-free; seq_cst fence prevents a read crossing publication.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint32_t after=source.revision.load(std::memory_order_acquire);
        if(before==after&&!(after&1u)){out=copy;if(revision)*revision=after;return true;}
    }
    return false;
}
Envelope Model::getEnvelope(bool volume,int slot) const noexcept {
    Envelope e=defaultEnvelope(volume);readEnvelope(volume,slot,e);return e;
}
void Model::setEnvelope(bool volume,int slot,const Envelope& input) {
    slot=std::max(0,std::min(kSlotCount-1,slot));
    Envelope e=input;e.count=std::max(2,std::min(kMaxPoints,e.count));
    for(int i=0;i<e.count;++i) {
        e.points[i].x=clamp01(e.points[i].x);e.points[i].y=clamp01(e.points[i].y);
        e.points[i].tension=std::isfinite(e.points[i].tension)?std::max(-1.0f,std::min(1.0f,e.points[i].tension)):0;
        e.points[i].curve=std::max(0,std::min(11,e.points[i].curve));
    }
    std::stable_sort(e.points.begin(),e.points.begin()+e.count,[](const Point&a,const Point&b){return a.x<b.x;});
    e.points[0].x=0;e.points[e.count-1].x=1;e.name[kNameLength-1]=0;
    for(int i=0;i<kNameLength-1;++i)if(static_cast<unsigned char>(e.name[i])<32&&e.name[i]!=0)e.name[i]=' ';
    std::lock_guard<std::mutex> guard(writerMutex_);
    auto& target=envelopes_[slot+(volume?kSlotCount:0)];
    target.revision.fetch_add(1,std::memory_order_seq_cst);
    target.count.store(e.count,std::memory_order_relaxed);
    for(int i=0;i<kMaxPoints;++i) {
        target.points[i].x.store(e.points[i].x,std::memory_order_relaxed);
        target.points[i].y.store(e.points[i].y,std::memory_order_relaxed);
        target.points[i].tension.store(e.points[i].tension,std::memory_order_relaxed);
        target.points[i].curve.store(e.points[i].curve,std::memory_order_relaxed);
    }
    for(int i=0;i<kNameLength;++i)target.name[i].store(e.name[i],std::memory_order_relaxed);
    target.revision.fetch_add(1,std::memory_order_release);
}
void Model::resetPresets() {
    for(int bank=0;bank<2;++bank)for(int slot=0;slot<kSlotCount;++slot)setEnvelope(bank!=0,slot,makePreset(bank!=0,slot));
}

} // namespace beatweaver
