#pragma once

#include "dsp/MurmurEngine.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <vector>

namespace murmur
{
// Murmur turns a sung, hummed or beatboxed voice into MIDI. It is a VST3 *audio effect*:
// stereo or mono audio in, the audio passed through unchanged, MIDI out. In Live it sits
// on the vocal audio track; a MIDI track picks it as "MIDI From" (docs/ABLETON_LIVE.md).
//
// Threading: processBlock (audio thread) mixes the input to mono, feeds the engine, and
// drains its MIDI into the block. The editor (message thread) reads the engine's atomic
// feedback getters and starts/clears drum calibration. Parameters flow APVTS -> engine
// as plain floats once per block. The audio thread never allocates after prepareToPlay.
class MurmurProcessor : public juce::AudioProcessor
{
public:
    MurmurProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Murmur"; }
    bool acceptsMidi() const override { return true; } // Live refuses a plugin with no MIDI input
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Editor access. The engine's getters are atomic; calibration calls are message-thread.
    dsp::MurmurEngine& getEngine() { return engine; }
    bool isHostPlaying() const { return hostPlaying.load(); }
    bool isDrumsMode() const { return apvts.getRawParameterValue("mode")->load() > 0.5f; }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void pushParametersToEngine();

    dsp::MurmurEngine engine;
    std::vector<float> mono;
    std::vector<dsp::MurmurEngine::MidiOut> midiScratch;
    juce::int64 samplePosition = 0;
    int lastMode = -1;
    int lastLatency = -1;
    std::atomic<bool> hostPlaying { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MurmurProcessor)
};
} // namespace murmur
