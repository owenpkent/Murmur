#include "MurmurHostProcessor.h"
#include "MurmurHostEditor.h"
#include <okstudio/StateHelpers.h>
#include <algorithm>

namespace murmur
{
MurmurHostProcessor::MurmurHostProcessor() : MurmurProcessor(true)
{
    formatManager.addFormat(new juce::VST3PluginFormat());
}

MurmurHostProcessor::~MurmurHostProcessor()
{
    // The editor (and with it the instrument's GUI) is always destroyed first by the wrapper.
    instrument = nullptr;
}

juce::String MurmurHostProcessor::instrumentName() const
{
    return instrument != nullptr ? instrument->getName() : juce::String();
}

double MurmurHostProcessor::getTailLengthSeconds() const
{
    return instrument != nullptr ? instrument->getTailLengthSeconds() : 0.0;
}

void MurmurHostProcessor::resizeHostBuffers(int samplesPerBlock)
{
    int channels = 2;
    if (instrument != nullptr)
        channels = juce::jmax(channels, instrument->getTotalNumInputChannels(),
                              instrument->getTotalNumOutputChannels());
    hostBuffer.setSize(channels, juce::jmax(1, samplesPerBlock));
    dryBlock.setSize(2, juce::jmax(1, samplesPerBlock));
}

void MurmurHostProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    MurmurProcessor::prepareToPlay(sampleRate, samplesPerBlock);
    instrumentMidi.ensureSize(4096); // grown here so the audio-thread copy doesn't allocate
    if (instrument != nullptr)
    {
        instrument->releaseResources();
        instrument->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
        instrument->prepareToPlay(sampleRate, samplesPerBlock);
        setExtraLatency(instrument->getLatencySamples());
    }
    resizeHostBuffers(samplesPerBlock);
    dryDelay.setSize(2, juce::jmax(1, (int) sampleRate)); // up to one second of compensation
    dryDelay.clear();
    dryWritePos = 0;
    lastMix = apvts.getRawParameterValue("mix")->load();
}

void MurmurHostProcessor::releaseResources()
{
    if (instrument != nullptr)
        instrument->releaseResources();
}

void MurmurHostProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    auto* inst = instrument.get();
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    const bool canHost = inst != nullptr && numSamples <= hostBuffer.getNumSamples()
                         && hostBuffer.getNumChannels() > 0 && numSamples <= dryBlock.getNumSamples()
                         && dryDelay.getNumSamples() > 1 && numCh <= 2;

    if (canHost) // keep the voice: Murmur only reads it, but the crossfade needs it afterwards
        for (int ch = 0; ch < numCh; ++ch)
            dryBlock.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    // Murmur: voice in, engine MIDI out, audio untouched.
    MurmurProcessor::processBlock(buffer, midi);

    const float mixTarget = apvts.getRawParameterValue("mix")->load();
    if (! canHost)
    {
        lastMix = mixTarget;
        return; // no instrument (or an oversized block): the voice passes through
    }

    juce::AudioBuffer<float> proxy(hostBuffer.getArrayOfWritePointers(),
                                   hostBuffer.getNumChannels(), numSamples);
    proxy.clear();
    instrumentMidi.clear();
    instrumentMidi.addEvents(midi, 0, numSamples, 0);
    inst->processBlock(proxy, instrumentMidi);

    // out = dry * (1 - mix) + instrument * mix, mix ramped across the block.
    const int ringSize = dryDelay.getNumSamples();
    const int delay = juce::jlimit(0, ringSize - 1, getLatencySamples());
    const float startMix = lastMix;
    const float step = (mixTarget - startMix) / (float) numSamples;
    for (int ch = 0; ch < numCh; ++ch)
    {
        const int srcCh = instrumentOutChannels == 1 ? 0 : juce::jmin(ch, proxy.getNumChannels() - 1);
        const float* dry = dryBlock.getReadPointer(ch);
        const float* ins = proxy.getReadPointer(srcCh);
        float* ring = dryDelay.getWritePointer(ch);
        float* out = buffer.getWritePointer(ch);
        int w = dryWritePos;
        float m = startMix;
        for (int i = 0; i < numSamples; ++i)
        {
            ring[w] = dry[i];
            int r = w - delay;
            if (r < 0)
                r += ringSize;
            m += step;
            out[i] = ring[r] * (1.0f - m) + ins[i] * m;
            if (++w >= ringSize)
                w = 0;
        }
    }
    dryWritePos = (dryWritePos + numSamples) % ringSize;
    lastMix = mixTarget;
}

juce::String MurmurHostProcessor::loadInstrument(const juce::File& vst3File)
{
    return loadInstrumentInternal(vst3File, nullptr);
}

juce::String MurmurHostProcessor::loadInstrumentInternal(const juce::File& vst3File,
                                                         const juce::MemoryBlock* stateToApply)
{
    JUCE_ASSERT_MESSAGE_THREAD
    loadError.clear();

    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType(vst3File.getFullPathName()))
            format->findAllTypesForFile(types, vst3File.getFullPathName());

    if (types.isEmpty())
    {
        loadError = "No VST3 plugin found in " + vst3File.getFileName();
        sendChangeMessage();
        return loadError;
    }

    const juce::PluginDescription* chosen = nullptr;
    for (auto* d : types)
        if (d->isInstrument)
        {
            chosen = d;
            break;
        }
    if (chosen == nullptr)
    {
        loadError = vst3File.getFileNameWithoutExtension() + " is not an instrument";
        sendChangeMessage();
        return loadError;
    }

    const double sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;
    const int bs = getBlockSize() > 0 ? getBlockSize() : 512;
    juce::String error;
    auto inst = formatManager.createPluginInstance(*chosen, sr, bs, error);
    if (inst == nullptr)
    {
        loadError = error.isNotEmpty() ? error : "Could not open " + vst3File.getFileName();
        sendChangeMessage();
        return loadError;
    }

    attachInstrument(std::move(inst), vst3File, stateToApply);
    return {};
}

void MurmurHostProcessor::ejectInstrument()
{
    JUCE_ASSERT_MESSAGE_THREAD
    loadError.clear();
    missingFile.clear();
    missingState.clear();
    attachInstrument(nullptr, juce::File(), nullptr);
}

void MurmurHostProcessor::attachInstrument(std::unique_ptr<juce::AudioPluginInstance> inst,
                                           const juce::File& file,
                                           const juce::MemoryBlock* stateToApply)
{
    if (onInstrumentWillChange)
        onInstrumentWillChange(); // the editor closes the old instrument GUI first

    suspendProcessing(true);
    if (instrument != nullptr)
        instrument->releaseResources();
    instrument = std::move(inst);
    instrumentFile = file;
    instrumentOutChannels = 0;

    if (instrument != nullptr)
    {
        missingFile.clear();
        missingState.clear();

        // Best effort: ask for a stereo main out; if the plugin refuses we copy its first channels.
        auto layout = instrument->getBusesLayout();
        if (! layout.outputBuses.isEmpty())
        {
            layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
            instrument->setBusesLayout(layout);
        }

        if (stateToApply != nullptr && stateToApply->getSize() > 0)
            instrument->setStateInformation(stateToApply->getData(), (int) stateToApply->getSize());

        const double sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;
        const int bs = getBlockSize() > 0 ? getBlockSize() : 512;
        instrument->setRateAndBufferSizeDetails(sr, bs);
        instrument->prepareToPlay(sr, bs);
        instrumentOutChannels = instrument->getTotalNumOutputChannels();
        resizeHostBuffers(bs);
        setExtraLatency(instrument->getLatencySamples());
    }
    else
    {
        setExtraLatency(0);
    }
    suspendProcessing(false);

    updateHostDisplay();
    sendChangeMessage();
}

void MurmurHostProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree hosted { "hostedInstrument" };
    if (instrument != nullptr)
    {
        hosted.setProperty("file", instrumentFile.getFullPathName(), nullptr);
        hosted.setProperty("name", instrument->getName(), nullptr);
        juce::MemoryBlock blob;
        instrument->getStateInformation(blob);
        hosted.setProperty("state", blob.toBase64Encoding(), nullptr);
    }
    else if (missingFile.isNotEmpty())
    {
        // A restored instrument that was not found: keep its path and state so saving the
        // project does not lose them, and it loads once the plugin is installed again.
        hosted.setProperty("file", missingFile, nullptr);
        hosted.setProperty("state", missingState, nullptr);
    }
    okstudio::state::save(apvts, "MURMUR", destData, { calibrationToTree(), hosted });
}

void MurmurHostProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    okstudio::state::load(apvts, data, sizeInBytes, [this](const juce::ValueTree& root)
    {
        restoreCalibration(root);

        const auto hosted = root.getChildWithName("hostedInstrument");
        if (! hosted.isValid() || hosted.getProperty("file").toString().isEmpty())
        {
            if (instrument != nullptr || missingFile.isNotEmpty())
                ejectInstrument();
            return;
        }

        const juce::File file(hosted.getProperty("file").toString());
        if (! file.exists())
        {
            // Survive a missing plugin: audio keeps passing, the editor says so.
            if (instrument != nullptr)
                ejectInstrument();
            missingFile = file.getFullPathName();
            missingState = hosted.getProperty("state").toString();
            loadError = "Instrument not found: " + file.getFullPathName();
            sendChangeMessage();
            return;
        }
        juce::MemoryBlock blob;
        blob.fromBase64Encoding(hosted.getProperty("state").toString());
        loadInstrumentInternal(file, &blob);
    });
}

juce::AudioProcessorEditor* MurmurHostProcessor::createEditor()
{
    return new MurmurHostEditor(*this);
}
} // namespace murmur

#if defined(MURMUR_HOST) && MURMUR_HOST
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new murmur::MurmurHostProcessor();
}
#endif
