#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "mcp/MurmurMcp.h"
#include <okstudio/Scales.h>
#include <okstudio/StateHelpers.h>
#include <algorithm>

namespace murmur
{
namespace
{
    juce::StringArray gridChoices() { return { "1/4", "1/8", "1/16", "1/8T", "1/16T", "1/32" }; }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout MurmurProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "mode", 1 }, "Mode",
                                                       StringArray { "Melody", "Drums" }, 0));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { "sensitivity", 1 }, "Sensitivity",
                                                      NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.5f));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { "quantize", 1 }, "Quantize",
                                                      NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.7f));
    layout.add(std::make_unique<AudioParameterFloat>(ParameterID { "swing", 1 }, "Swing",
                                                      NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "grid", 1 }, "Grid", gridChoices(), 2));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "root", 1 }, "Root",
                                                       okstudio::scales::noteNames(), 0));
    layout.add(std::make_unique<AudioParameterChoice>(ParameterID { "scale", 1 }, "Scale",
                                                       okstudio::scales::names(), 0));
    // Scale lock defaults ON: staying in key is the point of humming a melody.
    layout.add(std::make_unique<AudioParameterBool>(ParameterID { "scaleLock", 1 }, "Scale Lock", true));
    return layout;
}

MurmurProcessor::MurmurProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    mcpBridge = std::make_unique<MurmurMcp>(*this);
}

// The bridge holds a reference to this processor and its server thread calls back into it,
// so it goes first, before the members it reads are destroyed.
MurmurProcessor::~MurmurProcessor()
{
    mcpBridge.reset();
}

bool MurmurProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void MurmurProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int maxBlock = std::max(samplesPerBlock, 1);
    mono.assign((size_t) maxBlock, 0.0f);
    midiScratch.clear();
    midiScratch.reserve(256);
    engine.prepare(sampleRate, maxBlock);
    engine.reset();
    samplePosition = 0;
    lastMode = -1;
    lastLatency = engine.getLatencySamples();
    setLatencySamples(lastLatency);
}

void MurmurProcessor::pushParametersToEngine()
{
    auto get = [this](const char* id) { return apvts.getRawParameterValue(id)->load(); };

    const int mode = (int) get("mode");
    if (mode != lastMode)
    {
        engine.setMode(mode == 1 ? dsp::MurmurEngine::Mode::Drums : dsp::MurmurEngine::Mode::Melody);
        lastMode = mode;
    }
    engine.setSensitivity(get("sensitivity"));
    engine.setQuantizeStrength(get("quantize"));
    engine.setSwing(get("swing"));
    engine.setGridDivision((int) get("grid"));
    engine.setScale((int) get("root"), (int) get("scale"));
    engine.setScaleLock(get("scaleLock") > 0.5f);
}

void MurmurProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear(); // we author the whole outgoing MIDI stream from the voice.

    // Audio is passed through untouched: we only read the buffer.
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    double ppq = 0.0, bpm = 120.0;
    bool playing = false;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (const auto b = pos->getBpm())
                bpm = *b;
            if (const auto q = pos->getPpqPosition())
                ppq = *q;
        }
    }
    hostPlaying.store(playing);
    hostBpm.store(bpm);

    pushParametersToEngine();
    engine.setTransport(ppq, bpm, playing);

    const int chunkMax = (int) mono.size();
    for (int start = 0; start < numSamples && chunkMax > 0; start += chunkMax)
    {
        const int n = std::min(chunkMax, numSamples - start);
        if (numCh >= 2)
        {
            const float* l = buffer.getReadPointer(0) + start;
            const float* r = buffer.getReadPointer(1) + start;
            for (int i = 0; i < n; ++i)
                mono[(size_t) i] = 0.5f * (l[i] + r[i]);
        }
        else if (numCh == 1)
        {
            std::copy_n(buffer.getReadPointer(0) + start, n, mono.begin());
        }
        else
        {
            std::fill_n(mono.begin(), n, 0.0f);
        }

        engine.process(mono.data(), n, samplePosition + start);
        midiScratch.clear();
        engine.drainMidi(midiScratch);
        for (const auto& m : midiScratch)
            midi.addEvent(m.msg, start + std::clamp(m.sampleOffset, 0, n - 1));
    }
    samplePosition += numSamples;

    const int latency = engine.getLatencySamples();
    if (latency != lastLatency)
    {
        lastLatency = latency;
        setLatencySamples(latency);
    }
}

void MurmurProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // The calibration is wrapped so the engine's own tree type name stays its business.
    juce::ValueTree calib { "MURMUR_CALIBRATION" };
    const auto engineTree = engine.calibrationToValueTree();
    if (engineTree.isValid())
        calib.appendChild(engineTree, nullptr);
    okstudio::state::save(apvts, "MURMUR", destData, { calib });
}

void MurmurProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    okstudio::state::load(apvts, data, sizeInBytes, [this](const juce::ValueTree& root)
    {
        const auto calib = root.getChildWithName("MURMUR_CALIBRATION");
        if (calib.isValid() && calib.getNumChildren() > 0)
            engine.calibrationFromValueTree(calib.getChild(0));
    });
}

juce::AudioProcessorEditor* MurmurProcessor::createEditor()
{
    return new MurmurEditor(*this);
}
} // namespace murmur

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new murmur::MurmurProcessor();
}
