#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

// VoicePlayer: plays the corrected take as the voice (docs/TAKE_EDITOR_DESIGN.md, Phase B).
//
// Thread contract, the same handoff as TakePlayer and TakeCapture: publish() on the message
// thread only; process() on the audio thread, no lock, no allocation, no free. publish() puts the
// new buffer in the spare one of two slots and swaps the active pointer, then waits out any
// process() in flight (the in-use flag). The slots own the buffers through shared_ptr, so a
// buffer is only released here, on the message thread, when its slot is refilled two publishes
// later, long after the audio thread moved off it. A null buffer means "no audio": the voice is
// silent while a take plays.
//
// process() cross-fades whatever is in the block (the live input, or silence in the Standalone)
// with the corrected take. The gain ramps over a few ms toward 1 while a take plays and back to
// 0 after, so starting and stopping never clicks, and when the gain is 0 the block is not
// touched at all. After the take stops the voice keeps reading on for the length of the fade.
namespace murmur::take
{
class VoicePlayer
{
public:
    using Audio = std::shared_ptr<const std::vector<float>>;

    static constexpr double rampSeconds = 0.005;

    // Message thread. `sampleRate` is the audio's; at any other rate it would play at the wrong
    // pitch, so it is treated as no audio.
    void publish(Audio audio, double sampleRate)
    {
        auto& s = slots[spare];
        s.owner = std::move(audio);
        s.data = s.owner != nullptr ? s.owner->data() : nullptr;
        s.length = s.owner != nullptr ? (juce::int64) s.owner->size() : 0;
        s.rate = sampleRate;

        active.store(&s);
        spare = (&slots[0] == &s) ? 1 : 0;
        while (inUse.load())
            std::this_thread::yield();
    }

    // Audio thread. `takePos` is the take position (samples) of the block's first sample while
    // `playing`; the voice sounds `latency` samples later, as the MIDI does.
    void process(juce::AudioBuffer<float>& buffer, bool playing, juce::int64 takePos, juce::int64 latency,
                 double sampleRate) noexcept
    {
        if (! playing && gain <= 0.0f)
            return;
        inUse.store(true);
        const auto* s = active.load();
        const float* data = s != nullptr && s->rate == sampleRate ? s->data : nullptr;
        const juce::int64 length = data != nullptr ? s->length : 0;

        if (playing)
            nextPos = takePos;
        const int n = buffer.getNumSamples(), channels = buffer.getNumChannels();
        const float target = playing ? 1.0f : 0.0f;
        const float step = 1.0f / (float) std::max(1.0, std::round(rampSeconds * sampleRate));
        for (int i = 0; i < n; ++i)
        {
            gain = gain < target ? std::min(target, gain + step) : std::max(target, gain - step);
            const juce::int64 idx = nextPos + i - latency;
            const float v = idx >= 0 && idx < length ? data[idx] : 0.0f;
            for (int ch = 0; ch < channels; ++ch)
            {
                float& x = buffer.getWritePointer(ch)[i];
                x = x * (1.0f - gain) + v * gain;
            }
        }
        nextPos += n;
        inUse.store(false);
    }

    // Audio thread. True while the voice is audible (a take plays or its fade-out runs).
    bool isActive() const noexcept { return gain > 0.0f; }

private:
    struct Slot
    {
        Audio owner;
        const float* data = nullptr;
        juce::int64 length = 0;
        double rate = 0.0;
    };

    std::array<Slot, 2> slots;
    int spare = 1; // message thread only
    std::atomic<Slot*> active { nullptr };
    std::atomic<bool> inUse { false };

    // Audio thread only.
    float gain = 0.0f;
    juce::int64 nextPos = 0;
};
} // namespace murmur::take
