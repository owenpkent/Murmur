#pragma once

#include "../PluginProcessor.h"
#include <functional>
#include <memory>

namespace murmur
{
// Murmur Host: Murmur plus one hosted instrument VST3, in one audio-effect plugin on one
// track. The voice comes in, MurmurEngine turns it into MIDI (exactly as in plain Murmur),
// and that MIDI drives the hosted instrument, whose audio is the output. The "mix"
// parameter crossfades the (latency-aligned) voice and the instrument. The MIDI also stays
// in the host MidiBuffer so external routing ("MIDI From Murmur Host") keeps working.
// No instrument loaded behaves exactly like Murmur: audio passes through.
//
// Threading contract (same as Keys Host): the audio thread only reads `instrument` and never
// allocates or locks. Loading and ejecting happen on the message thread bracketed by
// suspendProcessing(), so a swap is never concurrent with processBlock.
class MurmurHostProcessor : public MurmurProcessor,
                            public juce::ChangeBroadcaster
{
public:
    MurmurHostProcessor();
    ~MurmurHostProcessor() override;

    const juce::String getName() const override { return "Murmur Host"; }
    juce::String productName() const override { return "Murmur Host"; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override;

    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Message thread only. Instruments only: a VST3 with no instrument inside is refused.
    // Returns an error message, empty on success.
    juce::String loadInstrument(const juce::File& vst3File);
    void ejectInstrument();

    bool hasInstrument() const noexcept { return instrument != nullptr; }
    juce::AudioPluginInstance* instrumentInstance() noexcept { return instrument.get(); }
    juce::String instrumentName() const;
    juce::File instrumentPath() const { return instrumentFile; }
    juce::String lastError() const { return loadError; }

    // The editor embeds the instrument's GUI, which must be torn down before the instance
    // it belongs to goes away. Called on the message thread just before the swap.
    std::function<void()> onInstrumentWillChange;

private:
    juce::String loadInstrumentInternal(const juce::File& vst3File, const juce::MemoryBlock* stateToApply);
    void attachInstrument(std::unique_ptr<juce::AudioPluginInstance>, const juce::File&,
                          const juce::MemoryBlock* stateToApply);
    void resizeHostBuffers(int samplesPerBlock);

    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> instrument;
    juce::File instrumentFile;
    juce::String loadError;
    juce::String missingFile, missingState; // a saved instrument that was not found on load

    // Scratch the instrument renders into; sized on prepare/load so processBlock can hand
    // the instrument its full channel count while our bus stays stereo or mono.
    juce::AudioBuffer<float> hostBuffer;
    int instrumentOutChannels = 0;

    // The instrument gets a copy of the block's MIDI so a synth that clears its buffer
    // cannot swallow the notes we also send out of the track.
    juce::MidiBuffer instrumentMidi;

    // The dry voice for the Mix crossfade, delayed by the plugin's reported latency so it
    // lines up with the instrument once the host applies delay compensation.
    juce::AudioBuffer<float> dryDelay; // ring, one second per channel
    juce::AudioBuffer<float> dryBlock; // the block's input, copied before Murmur touches it
    int dryWritePos = 0;
    float lastMix = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MurmurHostProcessor)
};
} // namespace murmur
