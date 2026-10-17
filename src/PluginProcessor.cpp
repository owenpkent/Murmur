#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "mcp/MurmurMcp.h"
#include "take/TakeAnalyser.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <okstudio/Scales.h>
#include <okstudio/StateHelpers.h>
#include <algorithm>

namespace murmur
{
namespace
{
    juce::StringArray gridChoices() { return { "1/4", "1/8", "1/16", "1/8T", "1/16T", "1/32" }; }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout MurmurProcessor::createLayout(bool hostLayout)
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
    // Murmur Host only: 0 = the voice passes through, 1 = only the hosted instrument is heard.
    if (hostLayout)
        layout.add(std::make_unique<AudioParameterFloat>(ParameterID { "mix", 1 }, "Mix",
                                                          NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));
    return layout;
}

MurmurProcessor::MurmurProcessor() : MurmurProcessor(false) {}

MurmurProcessor::MurmurProcessor(bool hostLayout)
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout(hostLayout)),
      takesFolder(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                      .getChildFile("OK Studio").getChildFile("Murmur").getChildFile("Takes"))
{
    mcpBridge = std::make_unique<MurmurMcp>(*this);
}

// The bridge holds a reference to this processor and its server thread calls back into it,
// so it goes first, before the members it reads are destroyed.
MurmurProcessor::~MurmurProcessor()
{
    mcpBridge.reset();
    // Jobs capture only copies, but they store into `pending` and trigger this updater, so
    // they are drained and the updater cancelled before any member goes away.
    cancelPendingUpdate();
    takePool.removeAllJobs(true, 10000);
    cancelPendingUpdate();
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
    playbackMidi.ensureSize(16384);
    engineHeld.fill(false);
    engine.prepare(sampleRate, maxBlock);
    engine.reset();
    samplePosition = 0;
    lastMode = -1;
    lastLatency = engine.getLatencySamples();
    setLatencySamples(lastLatency + extraLatency.load());
    player.publish(take, sampleRate); // the snapshot is in samples at this rate
}

void MurmurProcessor::setExtraLatency(int samples)
{
    extraLatency.store(std::max(0, samples));
    setLatencySamples(engine.getLatencySamples() + extraLatency.load());
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

    // Audio is passed through untouched: we only read the buffer. The one exception is the
    // Standalone app, where the voice is silenced at the end (see StandaloneSupport.h).
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

    // Take playback: never while recording. When it plays, its MIDI replaces the engine's (the
    // engine still analyses the audio); notes the engine had sounding are released first.
    playbackMidi.clear();
    const bool tookOver = player.render(playbackMidi, numSamples, engine.getLatencySamples(),
                                        playing && ! capture.isRecording(), ppq, getSampleRate());
    if (tookOver)
        for (int n = 0; n < 128; ++n)
            if (engineHeld[(size_t) n])
            {
                engineHeld[(size_t) n] = false;
                midi.addEvent(juce::MidiMessage::noteOff(1, n), 0);
            }

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

        capture.write(mono.data(), n, playing,
                      ppq + (double) start / std::max(getSampleRate(), 1.0) * bpm / 60.0, bpm);
        engine.process(mono.data(), n, samplePosition + start);
        midiScratch.clear();
        engine.drainMidi(midiScratch);
        if (! tookOver)
            for (const auto& m : midiScratch)
            {
                if (m.msg.isNoteOn())
                    engineHeld[(size_t) (m.msg.getNoteNumber() & 127)] = true;
                else if (m.msg.isNoteOff())
                    engineHeld[(size_t) (m.msg.getNoteNumber() & 127)] = false;
                midi.addEvent(m.msg, start + std::clamp(m.sampleOffset, 0, n - 1));
            }
    }
    samplePosition += numSamples;
    if (! playbackMidi.isEmpty())
        midi.addEvents(playbackMidi, 0, numSamples, 0);

    // Standalone only: a mic and speakers on one machine would feed back, so the voice is not
    // played out. Murmur Host still adds its instrument on top of this silence.
    if (wrapperType == wrapperType_Standalone)
        buffer.clear();

    const int latency = engine.getLatencySamples();
    if (latency != lastLatency)
    {
        lastLatency = latency;
        setLatencySamples(latency + extraLatency.load());
    }
}

juce::ValueTree MurmurProcessor::calibrationToTree() const
{
    // The calibration is wrapped so the engine's own tree type name stays its business.
    juce::ValueTree calib { "MURMUR_CALIBRATION" };
    const auto engineTree = engine.calibrationToValueTree();
    if (engineTree.isValid())
        calib.appendChild(engineTree, nullptr);
    return calib;
}

void MurmurProcessor::restoreCalibration(const juce::ValueTree& root)
{
    const auto calib = root.getChildWithName("MURMUR_CALIBRATION");
    if (calib.isValid() && calib.getNumChildren() > 0)
        engine.calibrationFromValueTree(calib.getChild(0));
}

void MurmurProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    okstudio::state::save(apvts, "MURMUR", destData, { calibrationToTree(), takeToTree() });
}

void MurmurProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    okstudio::state::load(apvts, data, sizeInBytes, [this](const juce::ValueTree& root)
    {
        restoreCalibration(root);
        restoreTake(root);
    });
}

// ---- Take Editor ---------------------------------------------------------------------------

MurmurProcessor::AnalysisSetup MurmurProcessor::makeAnalysisSetup() const
{
    // Built on the message thread from the APVTS, the same mapping as pushParametersToEngine,
    // so a job never reads the engine's live params from another thread.
    auto get = [this](const char* id) { return apvts.getRawParameterValue(id)->load(); };
    AnalysisSetup s;
    s.params.rootPitchClass = (int) get("root");
    s.params.scaleIndex = (int) get("scale");
    s.params.scaleLock = get("scaleLock") > 0.5f;
    s.onsetSensitivity = get("sensitivity");
    return s;
}

void MurmurProcessor::startJob(std::function<PendingResult()> work)
{
    jobsRunning.fetch_add(1);
    takePool.addJob([this, work = std::move(work)]
    {
        auto result = work();
        {
            const juce::ScopedLock sl(pendingLock);
            pending = std::move(result);
        }
        triggerAsyncUpdate();
        jobsRunning.fetch_sub(1); // last: after this the destructor may proceed
    });
}

bool MurmurProcessor::startRecording()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (getSampleRate() <= 0.0)
        return false;
    player.stop(); // a take never plays while another is being recorded
    return capture.arm(getSampleRate());
}

void MurmurProcessor::stopRecording()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! capture.isRecording())
        return;
    auto rec = std::make_shared<take::TakeCapture::Recording>(capture.stop());
    if (rec->audio.empty())
        return;

    const int generation = takeGeneration.fetch_add(1) + 1;
    const auto setup = makeAnalysisSetup();
    const auto folder = takesFolder;
    startJob([rec, setup, folder, generation]
    {
        PendingResult r;
        r.generation = generation;
        r.take = take::analyse(rec->audio.data(), (int) rec->audio.size(), rec->sampleRate,
                               setup.params, setup.onsetSensitivity);
        r.take.anchorPpq = rec->anchorPpq;
        r.take.anchorBpm = rec->anchorBpm;

        const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
        const auto file = folder.getNonexistentChildFile("take-" + stamp, ".wav");
        bool saved = false;
        if (folder.createDirectory().wasOk())
            if (auto stream = file.createOutputStream())
            {
                juce::WavAudioFormat wav;
                if (auto* raw = wav.createWriterFor(stream.get(), rec->sampleRate, 1, 32, {}, 0))
                {
                    stream.release(); // the writer owns it now
                    std::unique_ptr<juce::AudioFormatWriter> writer(raw);
                    const float* chans[] = { rec->audio.data() };
                    saved = writer->writeFromFloatArrays(chans, 1, (int) rec->audio.size());
                }
            }
        if (saved)
            r.take.audioPath = file.getFullPathName();
        else
            r.error = "Could not save take audio to " + file.getFullPathName();
        return r;
    });
}

bool MurmurProcessor::pollCapture()
{
    if (capture.isRecording() && capture.capReached())
    {
        stopRecording();
        return true;
    }
    return false;
}

bool MurmurProcessor::waitForTakeAnalysis(int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) std::max(timeoutMs, 0);
    while (jobsRunning.load() > 0 && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep(2);
    handleUpdateNowIfNeeded();
    return jobsRunning.load() == 0;
}

void MurmurProcessor::bumpTakeVersion()
{
    takeVersion.fetch_add(1);
    player.publish(take, getSampleRate());
}

void MurmurProcessor::playTake(double fromSeconds)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (capture.isRecording() || take.notes.empty() || getSampleRate() <= 0.0)
        return;
    player.play(fromSeconds);
}

void MurmurProcessor::stopTake()
{
    JUCE_ASSERT_MESSAGE_THREAD
    player.stop();
}

void MurmurProcessor::handleAsyncUpdate()
{
    std::optional<PendingResult> r;
    {
        const juce::ScopedLock sl(pendingLock);
        r = std::move(pending);
        pending.reset();
    }
    if (! r.has_value() || r->generation != takeGeneration.load())
        return; // superseded by a newer record, clear or restore

    if (r->curveOnly)
    {
        if (r->take.audioPath == take.audioPath)
            take.curve = std::move(r->take.curve);
        if (r->error.isNotEmpty())
            takeError = r->error;
    }
    else
    {
        take = std::move(r->take);
        takeHistory.clear();
        takeError = r->error;
    }
    bumpTakeVersion();
}

void MurmurProcessor::editTake(const std::function<void(take::Take&)>& edit)
{
    takeHistory.push(take);
    edit(take);
    bumpTakeVersion();
}

bool MurmurProcessor::undoTake()
{
    if (! takeHistory.undo(take))
        return false;
    bumpTakeVersion();
    return true;
}

void MurmurProcessor::clearTake()
{
    takeGeneration.fetch_add(1); // a job still running must not bring the old take back
    take = {};
    takeHistory.clear();
    takeError.clear();
    bumpTakeVersion();
}

juce::ValueTree MurmurProcessor::takeToTree() const
{
    return take.isEmpty() ? juce::ValueTree { "MURMUR_TAKE" } : take::toValueTree(take);
}

void MurmurProcessor::restoreTake(const juce::ValueTree& root)
{
    const int generation = takeGeneration.fetch_add(1) + 1;
    take = take::fromValueTree(root.getChildWithName("MURMUR_TAKE"));
    takeHistory.clear();
    takeError.clear();
    bumpTakeVersion();
    if (take.audioPath.isEmpty())
        return;

    const juce::File file(take.audioPath);
    if (! file.existsAsFile())
    {
        takeError = "Take audio not found: " + take.audioPath;
        return;
    }

    // Only the curve is rebuilt: the saved notes and edits stay exactly as they were.
    const auto setup = makeAnalysisSetup();
    const auto path = take.audioPath;
    startJob([file, path, setup, generation]
    {
        PendingResult r;
        r.generation = generation;
        r.curveOnly = true;
        r.take.audioPath = path;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(file.createInputStream().release(), true));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            r.error = "Take audio could not be read: " + path;
            return r;
        }
        juce::AudioBuffer<float> buf(1, (int) reader->lengthInSamples);
        reader->read(&buf, 0, buf.getNumSamples(), 0, true, false);
        r.take = take::analyse(buf.getReadPointer(0), buf.getNumSamples(), reader->sampleRate,
                               setup.params, setup.onsetSensitivity);
        r.take.audioPath = path;
        return r;
    });
}

juce::AudioProcessorEditor* MurmurProcessor::createEditor()
{
    return new MurmurEditor(*this);
}
} // namespace murmur

// The Murmur Host target compiles these same sources with MURMUR_HOST=1 and provides its own
// createPluginFilter (src/host/MurmurHostProcessor.cpp).
#if ! (defined(MURMUR_HOST) && MURMUR_HOST)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new murmur::MurmurProcessor();
}
#endif
