#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "VstAbi.h"
#include "Core.h"
#include "Engine.h"
#include "Editor.h"
#include "State.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace beatweaver {
namespace {
HINSTANCE moduleHandle = nullptr;
constexpr int maxEvents = 1024;
void copyString(void* destination, const char* source, size_t capacity) noexcept {
    if (!destination || !capacity) return;
    const size_t length = (std::min)(std::strlen(source), capacity - 1);
    std::memcpy(destination, source, length);
    static_cast<char*>(destination)[length] = '\0';
}

class Plugin {
public:
    vstabi::Effect effect{};
    explicit Plugin(vstabi::Callback host) : engine_(model_), editor_(model_, automation, this), host_(host) {
        effect.magic = vstabi::Magic;
        effect.dispatcher = dispatch;
        effect.process = accumulating;
        effect.setParameter = setParameter;
        effect.getParameter = getParameter;
        effect.numPrograms = 1;
        effect.numParams = kParameterCount;
        effect.numInputs = effect.numOutputs = 2;
        effect.flags = vstabi::HasEditor | vstabi::CanReplacing | vstabi::ProgramChunks;
        effect.ioRatio = 1;
        effect.object = this;
        effect.uniqueID = 0x42775776; // BwWv, identity belongs to this implementation.
        effect.version = 10000;
        effect.processReplacing = replacing;
        copyString(programName_, "BeatWeaver Bank", sizeof(programName_));
        engine_.prepare(sampleRate_, blockSize_);
    }
    ~Plugin() { editor_.close(); }
    static Plugin* instance(vstabi::Effect* effect) noexcept {
        return effect ? static_cast<Plugin*>(effect->object) : nullptr;
    }
private:
    Model model_;
    Engine engine_;
    Editor editor_;
    vstabi::Callback host_;
    double sampleRate_ = 44100;
    int blockSize_ = 1024;
    vstabi::Rect rectangle_{0, 0, static_cast<int16_t>(Editor::height()), static_cast<int16_t>(Editor::width())};
    std::array<MidiEvent, maxEvents> midi_{};
    int midiCount_ = 0;
    std::vector<uint8_t> chunk_;
    char programName_[25]{};
    double fallbackSample_ = 0;

    static void automation(void* context, int index, float value) {
        auto& self = *static_cast<Plugin*>(context);
        if (index < 0 || index >= kParameterCount) return;
        self.model_.setParam(static_cast<Param>(index), value);
        if (self.host_) {
            self.host_(&self.effect, vstabi::BeginEdit, index, 0, nullptr, 0);
            self.host_(&self.effect, vstabi::Automate, index, 0, nullptr, value);
            self.host_(&self.effect, vstabi::EndEdit, index, 0, nullptr, 0);
        }
    }
    static float getParameter(vstabi::Effect* effect, int32_t index) {
        auto* self = instance(effect);
        return self && index >= 0 && index < kParameterCount ? self->model_.getParam(static_cast<Param>(index)) : 0;
    }
    static void setParameter(vstabi::Effect* effect, int32_t index, float value) {
        auto* self = instance(effect);
        if (self && index >= 0 && index < kParameterCount) self->model_.setParam(static_cast<Param>(index), value);
    }
    Transport transport(int samples) noexcept {
        Transport result;
        result.ppq = fallbackSample_ / sampleRate_ * 2.0;
        if (host_) {
            auto* time = reinterpret_cast<const vstabi::TimeInfo*>(host_(&effect, vstabi::GetTime, 0,
                vstabi::PpqValid | vstabi::TempoValid, nullptr, 0));
            if (time) {
                result.playing = (time->flags & vstabi::TransportPlaying) != 0;
                result.discontinuity = (time->flags & vstabi::TransportChanged) != 0;
                if ((time->flags & vstabi::TempoValid) && std::isfinite(time->tempo) && time->tempo > 0)
                    result.tempo = time->tempo;
                if ((time->flags & vstabi::PpqValid) && std::isfinite(time->ppqPos)) {
                    result.ppq = time->ppqPos; result.positionValid = true;
                }
            }
        }
        fallbackSample_ += samples;
        return result;
    }
    void process(float** inputs, float** outputs, int32_t samples) noexcept {
        if (!outputs || !outputs[0] || !outputs[1] || samples <= 0) { midiCount_ = 0; return; }
        sortMidi();
        engine_.process(inputs, outputs, samples, transport(samples), midi_.data(), midiCount_);
        midiCount_ = 0;
    }
    void sortMidi() noexcept {
        // Stable insertion sort preserves host note-off/note-on order at equal offsets.
        for (int i = 1; i < midiCount_; ++i) {
            const auto event = midi_[i];
            int j = i;
            while (j > 0 && midi_[j - 1].offset > event.offset) { midi_[j] = midi_[j - 1]; --j; }
            midi_[j] = event;
        }
    }
    static void replacing(vstabi::Effect* effect, float** inputs, float** outputs, int32_t samples) {
        if (auto* self = instance(effect)) self->process(inputs, outputs, samples);
    }
    static void accumulating(vstabi::Effect* effect, float** inputs, float** outputs, int32_t samples) {
        // Legacy additive callback, with bounded stack storage and no allocation.
        auto* self = instance(effect);
        if (!self || !outputs || !outputs[0] || !outputs[1] || samples <= 0) return;
        constexpr int capacity = 256;
        std::array<float, capacity> left{}, right{};
        std::array<MidiEvent, maxEvents> chunkEvents{};
        self->sortMidi();
        const Transport blockTransport = self->transport(samples);
        int eventCursor = 0;
        for (int done = 0; done < samples; done += capacity) {
            int count = (std::min)(capacity, samples - done);
            float* in[2] = {inputs && inputs[0] ? inputs[0] + done : nullptr,
                            inputs && inputs[1] ? inputs[1] + done : nullptr};
            float* out[2] = {left.data(), right.data()};
            int events = 0;
            while (eventCursor < self->midiCount_ && self->midi_[eventCursor].offset < done + count) {
                auto event = self->midi_[eventCursor++]; event.offset = (std::max)(0, event.offset - done);
                chunkEvents[events++] = event;
            }
            auto time = blockTransport;
            time.ppq += done / self->sampleRate_ * time.tempo / 60.0;
            if (done != 0) time.discontinuity = false;
            self->engine_.process(in, out, count, time, chunkEvents.data(), events);
            for (int i = 0; i < count; ++i) { outputs[0][done + i] += left[i]; outputs[1][done + i] += right[i]; }
        }
        self->midiCount_ = 0;
    }
    intptr_t receiveEvents(void* pointer) noexcept {
        if (!pointer) return 0;
        const auto* events = static_cast<const vstabi::Events*>(pointer);
        const int count = (std::clamp)(events->numEvents, 0, maxEvents);
        const auto* list = reinterpret_cast<vstabi::Event* const*>(static_cast<const uint8_t*>(pointer) + offsetof(vstabi::Events, events));
        for (int i = 0; i < count && midiCount_ < maxEvents; ++i) {
            if (!list[i] || list[i]->type != 1 || list[i]->byteSize < sizeof(vstabi::MidiEvent)) continue;
            const auto* event = reinterpret_cast<const vstabi::MidiEvent*>(list[i]);
            const int status = event->midiData[0] & 0xf0;
            if (status == 0x80 || status == 0x90) {
                midi_[midiCount_++] = {(std::max)(0, event->deltaFrames),
                    static_cast<uint8_t>(event->midiData[1] & 0x7f),
                    static_cast<uint8_t>(event->midiData[2] & 0x7f),
                    status == 0x90 && event->midiData[2] != 0};
            } else if (status == 0xb0 && (event->midiData[1] == 120 || event->midiData[1] == 123)) {
                for (int note = 0; note < 128 && midiCount_ < maxEvents; ++note)
                    midi_[midiCount_++] = {(std::max)(0, event->deltaFrames), static_cast<uint8_t>(note), 0, false};
            }
        }
        return 1;
    }
    static intptr_t dispatch(vstabi::Effect* effect, int32_t opcode, int32_t index,
                             intptr_t value, void* pointer, float option) {
        auto* self = instance(effect);
        if (!self) return 0;
        // Exceptions must never cross the host ABI boundary.
        try { return self->dispatchImpl(opcode, index, value, pointer, option); }
        catch (...) { return 0; }
    }
    intptr_t dispatchImpl(int32_t opcode, int32_t index, intptr_t value, void* pointer, float option) {
        using namespace vstabi;
        switch (opcode) {
        case Open: return 1;
        case Close: delete this; return 1;
        case SetProgram: return value == 0;
        case GetProgram: return 0;
        case SetProgramName: if (pointer) copyString(programName_, static_cast<const char*>(pointer), sizeof(programName_)); return 1;
        case GetProgramName: copyString(pointer, programName_, 24); return 1;
        case GetProgramNameIndexed: if (index != 0) return 0; copyString(pointer, programName_, 24); return 1;
        case GetParamName:
            if (index < 0 || index >= kParameterCount) return 0;
            copyString(pointer, parameterName(static_cast<Param>(index)), 8); return 1;
        case GetParamDisplay: {
            if (index < 0 || index >= kParameterCount) return 0;
            char text[64]{};
            parameterText(static_cast<Param>(index), model_.getParam(static_cast<Param>(index)), text, sizeof(text));
            copyString(pointer, text, 8); return 1;
        }
        case GetParamLabel: copyString(pointer, "", 8); return 1;
        case SetSampleRate:
            if (!std::isfinite(option) || option < 8000 || option > 384000) return 0;
            sampleRate_ = option; engine_.prepare(sampleRate_, blockSize_); fallbackSample_ = 0; return 1;
        case SetBlockSize:
            if (value < 1 || value > 1048576) return 0;
            blockSize_ = static_cast<int>(value); return 1;
        case MainsChanged:
            midiCount_ = 0; engine_.reset(); fallbackSample_ = 0; return 1;
        case EditGetRect:
            if (!pointer) return 0; *static_cast<Rect**>(pointer) = &rectangle_; return 1;
        case EditOpen: return editor_.open(static_cast<HWND>(pointer), moduleHandle) ? 1 : 0;
        case EditClose: editor_.close(); return 1;
        case EditIdle: return 1;
        case GetChunk:
            if (!pointer) return 0;
            chunk_ = encodeState(model_); *static_cast<void**>(pointer) = chunk_.data(); return static_cast<intptr_t>(chunk_.size());
        case SetChunk: {
            const bool restored = value > 0 && decodeState(model_, pointer, static_cast<size_t>(value));
            if (restored && host_) host_(&effect, UpdateDisplay, 0, 0, nullptr, 0);
            return restored ? 1 : 0;
        }
        case ProcessEvents: return receiveEvents(pointer);
        case GetInputProperties: case GetOutputProperties: {
            if (!pointer || index < 0 || index > 1) return 0;
            auto& pin = *static_cast<PinProperties*>(pointer); std::memset(&pin, 0, sizeof(pin));
            copyString(pin.label, index == 0 ? "Stereo Left" : "Stereo Right", sizeof(pin.label));
            copyString(pin.shortLabel, index == 0 ? "L" : "R", sizeof(pin.shortLabel));
            pin.flags = 1 | (index == 0 ? 2 : 0); return 1;
        }
        case GetPlugCategory: return 1; // Effect.
        case SetBypass: model_.setParam(Param::Bypass, value ? 1.0f : 0.0f); return 1;
        case GetEffectName: copyString(pointer, "BeatWeaver", 32); return 1;
        case GetVendorString: copyString(pointer, "Choniel Audio", 64); return 1;
        case GetProductString: copyString(pointer, "BeatWeaver", 64); return 1;
        case GetVendorVersion: return 10000;
        case CanDo:
            if (!pointer) return 0;
            if (std::strcmp(static_cast<const char*>(pointer), "receiveVstEvents") == 0 ||
                std::strcmp(static_cast<const char*>(pointer), "receiveVstMidiEvent") == 0 ||
                std::strcmp(static_cast<const char*>(pointer), "receiveVstTimeInfo") == 0 ||
                std::strcmp(static_cast<const char*>(pointer), "bypass") == 0) return 1;
            return 0;
        case GetTailSize: return static_cast<intptr_t>(sampleRate_ * 96); // Maximum prepared delay history.
        case GetVstVersion: return 2400;
        case BeginSetProgram: case EndSetProgram: case StartProcess: case StopProcess: return 1;
        case SetProcessPrecision: return value == 0 ? 1 : 0;
        case GetNumMidiInputChannels: return 16;
        case GetNumMidiOutputChannels: return 0;
        default: return 0;
        }
    }
};
}
}

extern "C" __declspec(dllexport) vstabi::Effect* VSTPluginMain(vstabi::Callback host) {
    if (!host) return nullptr;
    try {
        if (host(nullptr, vstabi::HostVersion, 0, 0, nullptr, 0) == 0) return nullptr;
        auto plugin = std::make_unique<beatweaver::Plugin>(host);
        return &plugin.release()->effect;
    } catch (...) { return nullptr; }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        beatweaver::moduleHandle = instance;
    }
    return TRUE;
}
