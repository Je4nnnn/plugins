#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace beatweaver {

constexpr int kSlotCount = 36;
constexpr int kMaxPoints = 64;
constexpr int kNameLength = 32;

// All parameters use [0,1]. Slot selection maps to round(value * 35).
enum class Param : uint32_t {
    TimeMix, VolumeMix, Attack, Release, Tension, TimeSlot, VolumeSlot,
    TriggerSync, PositionSync, Length, Link, KeyHeld, OneShot, Hold,
    HoldMode, Scratch, Smooth, RemoveDC, MidiEnable, Bypass, Output, Count
};
constexpr int kParameterCount = static_cast<int>(Param::Count);
enum class Curve : int {
    Linear = 0, Hold = 1, Smooth = 2, Tension = 3, DoubleCurve = 4,
    AltSingle = 5, AltDouble = 6, Stairs = 7, SmoothStairs = 8,
    Pulse = 9, Wave = 10, HalfSine = 11
};

struct Point {
    float x = 0.0f;
    float y = 0.0f;
    float tension = 0.0f;
    int curve = static_cast<int>(Curve::Linear);
};
struct Envelope {
    int count = 2;
    std::array<Point, kMaxPoints> points{};
    char name[kNameLength]{};
};

float clamp01(float value) noexcept;
float defaultParameter(Param param) noexcept;
const char* parameterName(Param param) noexcept;
void parameterText(Param param, float value, char* text, int capacity) noexcept;
int parameterSlot(float value) noexcept;
float slotParameter(int slot) noexcept;
double syncBeats(float value) noexcept; // 0=free; 1/16,1/8,1/4,1/2,1,2,4,8 beats.
double lengthBeats(float value) noexcept; // 1/4,1/2,1,2,4,8,16 beats; default 4.
Envelope defaultEnvelope(bool volume) noexcept;
Envelope makePreset(bool volume, int index) noexcept;
float evaluateEnvelope(const Envelope& envelope, float x) noexcept;

class Model {
public:
    Model();
    std::atomic<float> playhead{0.0f}, inputPeak{0.0f}, outputPeak{0.0f};
    float getParam(Param param) const noexcept;
    void setParam(Param param, float value) noexcept;
    Envelope getEnvelope(bool volume, int slot) const noexcept;
    // Bounded lock-free reader for the audio thread. On contention leaves out unchanged.
    bool readEnvelope(bool volume, int slot, Envelope& out, uint32_t* revision = nullptr) const noexcept;
    // Control-thread operation. Never call from process/processEvents.
    void setEnvelope(bool volume, int slot, const Envelope& envelope);
    void resetPresets();
    uint32_t envelopeRevision(bool volume, int slot) const noexcept;

private:
    struct AtomicPoint {
        std::atomic<float> x{0}, y{0}, tension{0};
        std::atomic<int> curve{0};
    };
    struct AtomicEnvelope {
        std::atomic<uint32_t> revision{0};
        std::atomic<int> count{2};
        std::array<AtomicPoint, kMaxPoints> points{};
        std::array<std::atomic<char>, kNameLength> name{};
    };
    std::array<std::atomic<float>, kParameterCount> params_{};
    std::array<AtomicEnvelope, kSlotCount * 2> envelopes_{};
    std::mutex writerMutex_;
};

} // namespace beatweaver
