#pragma once

#include "Core.h"
#include <vector>

namespace beatweaver {

struct Transport {
    double tempo = 120.0;
    double ppq = 0.0;
    bool playing = true;
    bool positionValid = false;
    bool discontinuity = false;
};

struct MidiEvent {
    int offset = 0;
    uint8_t note = 60;
    uint8_t velocity = 127;
    bool on = true;
};

class Engine {
public:
    explicit Engine(Model& model) noexcept;
    void prepare(double sampleRate, int maximumBlockSize);
    void reset() noexcept;
    // Events must be ordered by offset; processing never allocates or locks.
    void process(float** inputs, float** outputs, int samples, const Transport& transport,
                 const MidiEvent* events = nullptr, int eventCount = 0) noexcept;

private:
    void handleMidi(const MidiEvent& event, double beat) noexcept;
    void selectSlot(bool volume, int slot, double beat, bool force) noexcept;
    void updateEnvelope(bool volume) noexcept;
    void trigger(bool volume, int slot, double beat) noexcept;
    void startHold(size_t length) noexcept;
    float readDelay(int channel, double delaySamples) const noexcept;
    Model& model_;
    double sampleRate_ = 44100.0;
    std::array<std::vector<float>, 2> buffer_{};
    std::array<std::vector<float>, 2> holdBuffer_{};
    size_t writePosition_ = 0;
    size_t validSamples_ = 0, holdLength_ = 0, holdCopied_ = 0, holdStart_ = 0, holdPosition_ = 0, holdValid_ = 0;
    size_t holdCaptureRemaining_ = 0, holdCaptureLength_ = 0;
    int holdMode_ = 0;
    std::array<Envelope, 2> envelopes_{};
    std::array<int, 2> activeSlot_{{0,0}}, requestedSlot_{{0,0}}, pendingSlot_{{-1,-1}};
    std::array<uint32_t, 2> revisions_{{0,0}};
    std::array<double, 2> originBeat_{{0,0}}, pendingBeat_{{0,0}};
    std::array<double, 2> triggerBeat_{{0,0}};
    std::array<bool, 2> completed_{{false,false}};
    std::array<bool, 128> heldNotes_{};
    std::array<uint32_t, 128> noteOrder_{};
    std::array<int, 2> returnSlot_{{0,0}};
    uint32_t nextNoteOrder_ = 1;
    int heldCount_ = 0;
    double freeBeat_ = 0, previousEndBeat_ = 0, currentTempo_ = 120.0;
    double previousDelay_ = 0, fadeFromDelay_ = 0;
    float timeMix_ = 1, volumeMix_ = 1, outputGain_ = 1, gate_ = 1;
    float volumeGain_ = 1, previousVolume_ = 1;
    std::array<float, 2> dcInput_{{0,0}}, dcOutput_{{0,0}};
    int fadeRemaining_ = 0, fadeLength_ = 64;
    bool prepared_ = false, wasPlaying_ = false, holdActive_ = false, holdRequested_ = false;
};

} // namespace beatweaver
