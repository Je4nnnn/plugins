#include "Engine.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace beatweaver {
namespace {
constexpr double kMinimumTempo = 20.0;
constexpr double kMaximumTempo = 1000.0;
constexpr double kBufferSeconds = 96.0; // 2 x 16 beats at 20 BPM.
double fraction(double x) noexcept { return x-std::floor(x); }
float safeAudio(float x) noexcept { return std::isfinite(x)?x:0.0f; }
int noteBank(int note) noexcept { return note>=12&&note<48?1:note>=48&&note<84?0:-1; }
int noteSlot(int note) noexcept { return note>=48?note-48:note-12; }
}

Engine::Engine(Model& model) noexcept : model_(model) {
    envelopes_[0]=defaultEnvelope(false);envelopes_[1]=defaultEnvelope(true);
}
void Engine::prepare(double sampleRate,int maximumBlockSize) {
    sampleRate_=std::isfinite(sampleRate)?std::max(8000.0,std::min(384000.0,sampleRate)):44100.0;
    const size_t count=static_cast<size_t>(std::ceil(sampleRate_*kBufferSeconds))+8;
    for(int ch=0;ch<2;++ch) {
        buffer_[ch].assign(count,0);
        holdBuffer_[ch].assign(count/2,0);
    }
    (void)maximumBlockSize;
    prepared_=true;reset();
}
void Engine::reset() noexcept {
    // A valid-sample counter discards stale samples without clearing a large ring on a seek.
    writePosition_=validSamples_=holdLength_=holdCopied_=holdStart_=holdPosition_=holdValid_=0;
    holdCaptureRemaining_=holdCaptureLength_=0;holdActive_=holdRequested_=false;holdMode_=0;
    originBeat_={{0,0}};triggerBeat_={{0,0}};pendingBeat_={{0,0}};pendingSlot_={{-1,-1}};completed_={{false,false}};
    heldNotes_.fill(false);noteOrder_.fill(0);heldCount_=0;nextNoteOrder_=1;
    previousDelay_=fadeFromDelay_=freeBeat_=previousEndBeat_=0;
    timeMix_=model_.getParam(Param::TimeMix);volumeMix_=model_.getParam(Param::VolumeMix);
    outputGain_=2*model_.getParam(Param::Output);volumeGain_=previousVolume_=gate_=1;
    fadeRemaining_=0;dcInput_={{0,0}};dcOutput_={{0,0}};wasPlaying_=false;
    for(int bank=0;bank<2;++bank) {
        const int slot=parameterSlot(model_.getParam(bank?Param::VolumeSlot:Param::TimeSlot));
        activeSlot_[bank]=requestedSlot_[bank]=returnSlot_[bank]=slot;
        revisions_[bank]=std::numeric_limits<uint32_t>::max();
        model_.readEnvelope(bank!=0,slot,envelopes_[bank],&revisions_[bank]);
    }
    model_.playhead.store(0,std::memory_order_relaxed);
    model_.inputPeak.store(0,std::memory_order_relaxed);model_.outputPeak.store(0,std::memory_order_relaxed);
}
void Engine::updateEnvelope(bool volume) noexcept {
    const int bank=volume?1:0;
    const uint32_t revision=model_.envelopeRevision(volume,activeSlot_[bank]);
    if(revision!=revisions_[bank]&&!(revision&1u)) {
        model_.readEnvelope(volume,activeSlot_[bank],envelopes_[bank],&revisions_[bank]);
    }
}
void Engine::trigger(bool volume,int slot,double beat) noexcept {
    const int bank=volume?1:0;
    activeSlot_[bank]=std::max(0,std::min(kSlotCount-1,slot));
    const double position=syncBeats(model_.getParam(Param::PositionSync));
    originBeat_[bank]=position>0?std::floor((beat+1e-10)/position)*position:beat;
    triggerBeat_[bank]=beat;
    completed_[bank]=false;pendingSlot_[bank]=-1;
    revisions_[bank]=std::numeric_limits<uint32_t>::max();
    model_.readEnvelope(volume,activeSlot_[bank],envelopes_[bank],&revisions_[bank]);
}
void Engine::selectSlot(bool volume,int slot,double beat,bool force) noexcept {
    const int bank=volume?1:0;slot=std::max(0,std::min(kSlotCount-1,slot));
    if(!force&&slot==requestedSlot_[bank])return;
    requestedSlot_[bank]=slot;
    const double grid=syncBeats(model_.getParam(Param::TriggerSync));
    if(grid>0) {
        const double due=std::ceil((beat-1e-9)/grid)*grid;
        if(due>beat+1e-8){pendingSlot_[bank]=slot;pendingBeat_[bank]=due;return;}
    }
    trigger(volume,slot,beat);
}
void Engine::handleMidi(const MidiEvent& event,double beat) noexcept {
    if(model_.getParam(Param::MidiEnable)<.5f)return;
    const int note=event.note,bank=noteBank(note);
    if(bank<0)return;
    const bool on=event.on&&event.velocity>0;
    const bool keyHeld=model_.getParam(Param::KeyHeld)>=.5f;
    const bool linked=model_.getParam(Param::Link)>=.5f;
    const Param parameter=bank?Param::VolumeSlot:Param::TimeSlot;
    if(on) {
        bool hadHeld=false;
        for(int n=0;n<128;++n)if(heldNotes_[n]&&noteBank(n)==bank){hadHeld=true;break;}
        if(!hadHeld)returnSlot_[bank]=parameterSlot(model_.getParam(parameter));
        if(!heldNotes_[note])++heldCount_;
        heldNotes_[note]=true;noteOrder_[note]=nextNoteOrder_++;
        const int slot=noteSlot(note);
        model_.setParam(parameter,slotParameter(slot));selectSlot(bank!=0,slot,beat,true);
        if(linked) {
            if(!hadHeld)returnSlot_[1-bank]=parameterSlot(model_.getParam(bank?Param::TimeSlot:Param::VolumeSlot));
            model_.setParam(bank?Param::TimeSlot:Param::VolumeSlot,slotParameter(slot));
            selectSlot(bank==0,slot,beat,true);
        }
    } else {
        if(!heldNotes_[note])return;
        heldNotes_[note]=false;heldCount_=std::max(0,heldCount_-1);
        if(!keyHeld)return;
        int recent=-1;uint32_t order=0;
        for(int n=0;n<128;++n)if(heldNotes_[n]&&noteBank(n)==bank&&noteOrder_[n]>=order){order=noteOrder_[n];recent=n;}
        const int slot=recent<0?returnSlot_[bank]:noteSlot(recent);
        model_.setParam(parameter,slotParameter(slot));selectSlot(bank!=0,slot,beat,true);
        if(linked) {
            const int other=recent<0?returnSlot_[1-bank]:slot;
            model_.setParam(bank?Param::TimeSlot:Param::VolumeSlot,slotParameter(other));
            selectSlot(bank==0,other,beat,true);
        }
    }
}
void Engine::startHold(size_t length) noexcept {
    if(buffer_[0].empty())return;
    holdLength_=std::max<size_t>(2,std::min(length,holdBuffer_[0].size()));
    holdValid_=std::min(validSamples_,holdLength_);
    holdStart_=(writePosition_+buffer_[0].size()-holdLength_)%buffer_[0].size();
    holdCopied_=holdPosition_=0;holdActive_=true;
}
float Engine::readDelay(int channel,double delaySamples) const noexcept {
    const size_t size=buffer_[channel].size();
    if(size<4)return 0;
    delaySamples=std::isfinite(delaySamples)?std::max(0.0,std::min(double(size-3),delaySamples)):0;
    if(holdActive_&&holdLength_>1) {
        double position=double(holdPosition_)-delaySamples;
        position-=std::floor(position/double(holdLength_))*double(holdLength_);
        const size_t first=static_cast<size_t>(position),second=(first+1)%holdLength_;
        const float weight=float(position-first);
        const auto sample=[&](size_t index) noexcept {
            if(index<holdLength_-holdValid_)return 0.0f;
            return index<holdCopied_?holdBuffer_[channel][index]:buffer_[channel][(holdStart_+index)%size];
        };
        return sample(first)+(sample(second)-sample(first))*weight;
    }
    if(validSamples_==0||delaySamples>double(validSamples_-1))return 0;
    double position=double(writePosition_)-delaySamples;
    if(position<0)position+=double(size);
    const size_t first=static_cast<size_t>(position),second=(first+1)%size;
    const float weight=float(position-first);
    return buffer_[channel][first]+(buffer_[channel][second]-buffer_[channel][first])*weight;
}
void Engine::process(float** inputs,float** outputs,int samples,const Transport& transport,
                     const MidiEvent* events,int eventCount) noexcept {
    if(samples<=0||!outputs)return;
    if(!prepared_) {
        for(int ch=0;ch<2;++ch)if(outputs[ch])for(int i=0;i<samples;++i)
            outputs[ch][i]=inputs&&inputs[ch]?safeAudio(inputs[ch][i]):0;
        return;
    }
    std::array<float,kParameterCount> parameters{};
    for(int i=0;i<kParameterCount;++i)parameters[i]=model_.getParam(static_cast<Param>(i));
    const auto p=[&](Param param) noexcept {return parameters[static_cast<int>(param)];};
    const double tempo=std::isfinite(transport.tempo)?std::max(kMinimumTempo,std::min(kMaximumTempo,transport.tempo)):120;
    currentTempo_=tempo;
    const double beatPerSample=tempo/(60*sampleRate_);
    const bool validPosition=transport.positionValid&&std::isfinite(transport.ppq);
    const double blockBeat=validPosition?transport.ppq:freeBeat_;
    const bool seek=transport.discontinuity || (validPosition&&wasPlaying_&&transport.playing&&
        std::fabs(blockBeat-previousEndBeat_)>std::max(.025,beatPerSample*8));
    if(seek) {
        // Clear logical history and captured holds; do not touch buffer memory in the callback.
        validSamples_=0;holdActive_=holdRequested_=false;holdCaptureRemaining_=0;
        previousDelay_=0;fadeRemaining_=0;dcInput_={{0,0}};dcOutput_={{0,0}};
        completed_={{false,false}};pendingSlot_={{-1,-1}};requestedSlot_={{-1,-1}};
    }
    if(seek||(!wasPlaying_&&transport.playing)) {
        const double position=syncBeats(p(Param::PositionSync));
        for(int bank=0;bank<2;++bank) {
            originBeat_[bank]=position>0?std::floor((blockBeat+1e-10)/position)*position:blockBeat;
            triggerBeat_[bank]=blockBeat;
        }
    }
    const double length=lengthBeats(p(Param::Length));
    const double patternSamples=length/beatPerSample;
    const size_t captureLength=std::max<size_t>(2,std::min(static_cast<size_t>(patternSamples),holdBuffer_[0].size()));
    const int mode=int(std::lround(p(Param::HoldMode)*2));
    const bool wantHold=p(Param::Hold)>=.5f&&mode!=0;
    if(!wantHold){holdActive_=false;holdCaptureRemaining_=0;}
    else if(!holdRequested_||mode!=holdMode_) {
        holdActive_=false;
        if(mode==1)startHold(captureLength);
        else {holdCaptureRemaining_=holdCaptureLength_=captureLength;}
    }
    holdRequested_=wantHold;holdMode_=mode;
    const bool linked=p(Param::Link)>=.5f;
    int timeSlot=parameterSlot(p(Param::TimeSlot)),volumeSlot=parameterSlot(p(Param::VolumeSlot));
    if(linked) {
        // Either bank may be automated. If both change together, the time bank wins.
        if(timeSlot==requestedSlot_[0]&&volumeSlot!=requestedSlot_[1])timeSlot=volumeSlot;
        if(parameterSlot(p(Param::TimeSlot))!=timeSlot)model_.setParam(Param::TimeSlot,slotParameter(timeSlot));
        if(volumeSlot!=timeSlot)model_.setParam(Param::VolumeSlot,slotParameter(timeSlot));
        selectSlot(false,timeSlot,blockBeat,false);
        selectSlot(true,timeSlot,blockBeat,false);
    } else {
        selectSlot(false,timeSlot,blockBeat,false);
        selectSlot(true,volumeSlot,blockBeat,false);
    }
    updateEnvelope(false);updateEnvelope(true);

    fadeLength_=std::max(1,int(sampleRate_*(.0001+.020*p(Param::Smooth))));
    const float mixCoefficient=float(1-std::exp(-1/(sampleRate_*.003)));
    const float attack=p(Param::Attack)*.5f,release=p(Param::Release);
    const float attackCoefficient=attack<=.000001f?1.0f:float(1-std::exp(-1/(sampleRate_*attack)));
    const float releaseCoefficient=release<=.000001f?1.0f:float(1-std::exp(-1/(sampleRate_*release)));
    const float volumePower=std::pow(4.0f,(.5f-p(Param::Tension))*2);
    const float dcCoefficient=float(std::exp(-6.28318530717958647692*5/sampleRate_));
    const bool bypass=p(Param::Bypass)>=.5f,removeDC=p(Param::RemoveDC)>=.5f;
    const bool oneShot=p(Param::OneShot)>=.5f,keyHeld=p(Param::KeyHeld)>=.5f;
    const double phaseAdvance=(!validPosition||transport.playing)?beatPerSample:0;
    float inputPeak=0,outputPeak=0,lastPhase=0;
    int eventIndex=0;eventCount=events?std::max(0,eventCount):0;
    for(int i=0;i<samples;++i) {
        const double beat=blockBeat+i*phaseAdvance;
        while(eventIndex<eventCount&&events[eventIndex].offset<=i)handleMidi(events[eventIndex++],beat);
        for(int bank=0;bank<2;++bank)if(pendingSlot_[bank]>=0&&beat+1e-8>=pendingBeat_[bank])
            trigger(bank!=0,pendingSlot_[bank],pendingBeat_[bank]);
        std::array<float,2> phases{};
        for(int bank=0;bank<2;++bank) {
            const double elapsed=std::max(0.0,(beat-triggerBeat_[bank])/length);
            if(oneShot&&elapsed>=1&&!completed_[bank]) {
                bool held=false;
                if(keyHeld)for(int n=0;n<128;++n)if(heldNotes_[n]&&(linked||noteBank(n)==bank)){held=true;break;}
                if(!held) {
                    model_.setParam(bank?Param::VolumeSlot:Param::TimeSlot,0);
                    requestedSlot_[bank]=0;trigger(bank!=0,0,beat);
                } else completed_[bank]=true;
            }
            phases[bank]=completed_[bank]?1.0f:float(fraction(std::max(0.0,(beat-originBeat_[bank])/length)));
        }
        lastPhase=phases[0];
        // Copy at most eight stereo samples per output sample. The live ring is twice as
        // long as a captured phrase, so uncopied snapshot data cannot be overwritten.
        if(holdActive_)for(int copy=0;copy<8&&holdCopied_<holdLength_;++copy,++holdCopied_) {
            const size_t index=(holdStart_+holdCopied_)%buffer_[0].size();
            for(int ch=0;ch<2;++ch)holdBuffer_[ch][holdCopied_]=holdCopied_<holdLength_-holdValid_?0:buffer_[ch][index];
        }
        std::array<float,2> dry{};
        for(int ch=0;ch<2;++ch) {
            dry[ch]=inputs&&inputs[ch]?safeAudio(inputs[ch][i]):0;
            inputPeak=std::max(inputPeak,std::fabs(dry[ch]));
            buffer_[ch][writePosition_]=dry[ch];
        }
        validSamples_=std::min(buffer_[0].size()-1,validSamples_+1);
        const double desiredDelay=(double(evaluateEnvelope(envelopes_[0],phases[0]))+p(Param::Scratch))*2*patternSamples;
        const double delay=std::max(0.0,std::min(double(buffer_[0].size()-3),desiredDelay));
        if(std::fabs(delay-previousDelay_)>16&&fadeRemaining_==0){fadeFromDelay_=previousDelay_;fadeRemaining_=fadeLength_;}
        const float blend=fadeRemaining_>0?1-float(fadeRemaining_)/fadeLength_:1;
        const float rawVolume=std::pow(evaluateEnvelope(envelopes_[1],phases[1]),volumePower);
        volumeGain_+=(rawVolume-volumeGain_)*(rawVolume>volumeGain_?attackCoefficient:releaseCoefficient);
        if(std::fabs(volumeGain_)<1e-20f)volumeGain_=0;
        timeMix_+=(p(Param::TimeMix)-timeMix_)*mixCoefficient;
        volumeMix_+=(p(Param::VolumeMix)-volumeMix_)*mixCoefficient;
        outputGain_+=(p(Param::Output)*2-outputGain_)*mixCoefficient;
        const float amplitude=(1-volumeMix_)+volumeMix_*volumeGain_;
        for(int ch=0;ch<2;++ch) {
            float wet=readDelay(ch,delay);
            if(fadeRemaining_>0)wet=readDelay(ch,fadeFromDelay_)*(1-blend)+wet*blend;
            float value=(dry[ch]*(1-timeMix_)+wet*timeMix_)*amplitude*outputGain_;
            if(removeDC) {
                const float filtered=value-dcInput_[ch]+dcCoefficient*dcOutput_[ch];
                dcInput_[ch]=value;dcOutput_[ch]=std::fabs(filtered)<1e-20f?0:filtered;value=filtered;
            } else {dcInput_[ch]=value;dcOutput_[ch]=0;}
            if(bypass)value=dry[ch];
            value=safeAudio(value);if(outputs[ch])outputs[ch][i]=value;
            outputPeak=std::max(outputPeak,std::fabs(value));
        }
        previousDelay_=delay;if(fadeRemaining_>0)--fadeRemaining_;
        writePosition_=(writePosition_+1)%buffer_[0].size();
        if(holdActive_)holdPosition_=(holdPosition_+1)%holdLength_;
        if(holdCaptureRemaining_>0&&--holdCaptureRemaining_==0)startHold(holdCaptureLength_);
    }
    freeBeat_=blockBeat+samples*phaseAdvance;previousEndBeat_=freeBeat_;wasPlaying_=transport.playing;
    model_.playhead.store(lastPhase,std::memory_order_relaxed);
    model_.inputPeak.store(inputPeak,std::memory_order_relaxed);model_.outputPeak.store(outputPeak,std::memory_order_relaxed);
}

} // namespace beatweaver
