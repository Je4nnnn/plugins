#pragma once
// Minimal, independently declared binary interface for classic VST hosts.
// No third-party SDK implementation or headers are embedded in this project.
#include <cstdint>
#include <cstddef>

namespace vstabi {
struct Effect;
using Callback = intptr_t (*)(Effect*, int32_t, int32_t, intptr_t, void*, float);
using Process = void (*)(Effect*, float**, float**, int32_t);
using ProcessDouble = void (*)(Effect*, double**, double**, int32_t);
using SetParameter = void (*)(Effect*, int32_t, float);
using GetParameter = float (*)(Effect*, int32_t);

struct Effect {
    int32_t magic;
    Callback dispatcher;
    Process process;
    SetParameter setParameter;
    GetParameter getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t reserved1, reserved2;
    int32_t initialDelay, realtimeQualities, offlineQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID, version;
    Process processReplacing;
    ProcessDouble processDoubleReplacing;
    char future[56];
};
static_assert(sizeof(void*) == 8, "Build this plug-in for x64.");
static_assert(sizeof(Effect) == 192 && offsetof(Effect, processReplacing) == 120,
              "Unexpected classic VST ABI layout.");

struct Rect { int16_t top, left, bottom, right; };
struct TimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo;
    double barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator;
    int32_t smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
struct Event { int32_t type, byteSize, deltaFrames, flags; char data[16]; };
struct MidiEvent {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    uint8_t midiData[4];
    int8_t detune;
    uint8_t noteOffVelocity, reserved1, reserved2;
};
struct Events { int32_t numEvents; intptr_t reserved; Event* events[2]; };
struct PinProperties { char label[64]; int32_t flags, arrangementType; char shortLabel[8]; char future[48]; };

enum HostOpcode : int32_t {
    Automate = 0, HostVersion = 1, Idle = 3, GetTime = 7,
    IOChanged = 13, GetSampleRate = 16, GetBlockSize = 17,
    UpdateDisplay = 42, BeginEdit = 43, EndEdit = 44
};
enum Opcode : int32_t {
    Open = 0, Close = 1, SetProgram = 2, GetProgram = 3,
    SetProgramName = 4, GetProgramName = 5, GetParamLabel = 6,
    GetParamDisplay = 7, GetParamName = 8, SetSampleRate = 10,
    SetBlockSize = 11, MainsChanged = 12, EditGetRect = 13,
    EditOpen = 14, EditClose = 15, EditIdle = 19, GetChunk = 23,
    SetChunk = 24, ProcessEvents = 25, GetProgramNameIndexed = 29,
    GetInputProperties = 33, GetOutputProperties = 34, GetPlugCategory = 35,
    SetBypass = 44, GetEffectName = 45, GetVendorString = 47,
    GetProductString = 48, GetVendorVersion = 49, CanDo = 51,
    GetTailSize = 52, GetVstVersion = 58, BeginSetProgram = 67,
    EndSetProgram = 68, StartProcess = 71, StopProcess = 72,
    SetProcessPrecision = 77, GetNumMidiInputChannels = 78,
    GetNumMidiOutputChannels = 79
};
constexpr int32_t Magic = 0x56737450;
constexpr int32_t HasEditor = 1, CanReplacing = 16, ProgramChunks = 32;
constexpr int32_t TransportChanged = 1, TransportPlaying = 2;
constexpr int32_t PpqValid = 1 << 9, TempoValid = 1 << 10;
}
