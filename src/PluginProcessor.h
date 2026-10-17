#pragma once

#include "dsp/MurmurEngine.h"
#include "take/TakeCapture.h"
#include "take/TakeModel.h"
#include "take/TakePlayer.h"
#include "take/VoicePlayer.h"
#include "take/VoiceRenderer.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace murmur
{
class MurmurMcp; // src/mcp/MurmurMcp.h; only PluginProcessor.cpp needs the full type.

// Murmur turns a sung, hummed or beatboxed voice into MIDI. It is a VST3 *audio effect*:
// stereo or mono audio in, the audio passed through unchanged, MIDI out. In Live it sits
// on the vocal audio track; a MIDI track picks it as "MIDI From" (docs/ABLETON_LIVE.md).
//
// Threading: processBlock (audio thread) mixes the input to mono, feeds the engine, and
// drains its MIDI into the block. The editor (message thread) reads the engine's atomic
// feedback getters and starts/clears drum calibration. Parameters flow APVTS -> engine
// as plain floats once per block. The audio thread never allocates after prepareToPlay.
class MurmurProcessor : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    MurmurProcessor();
    ~MurmurProcessor() override;

    // The product name shown in the MCP snapshot. Murmur Host overrides it.
    virtual juce::String productName() const { return "Murmur"; }

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
    double getHostBpm() const { return hostBpm.load(); } // 120 until the host reports a tempo
    bool isDrumsMode() const { return apvts.getRawParameterValue("mode")->load() > 0.5f; }

    // The embedded MCP bridge (src/mcp/MurmurMcp.h) letting Claude Code or any local MCP
    // client read and drive Murmur. Owned here, one per processor; tests reach it through
    // this accessor rather than building a second one (which would bind a second port).
    MurmurMcp* mcp() const { return mcpBridge.get(); }

    // ---- Take Editor capture (docs/TAKE_EDITOR_DESIGN.md). Message thread unless noted. ----
    // False if not prepared (no sample rate) or already recording.
    bool startRecording();
    // Ends the capture and starts the background job: analyse, save the WAV, then the result
    // is installed as `take` on the message thread (handleAsyncUpdate).
    void stopRecording();
    // The editor timer calls this: stops the take if the buffer hit its cap. True if it did.
    bool pollCapture();
    // Blocks until no job is running, then installs any finished result. For tests and MCP.
    bool waitForTakeAnalysis(int timeoutMs);

    const take::Take& getTake() const { return take; }
    int getTakeVersion() const { return takeVersion.load(); } // bumps on every change; poll it
    take::TakeCapture& getCapture() { return capture; }
    juce::String getTakeError() const { return takeError; }
    // One undo entry per call: push history, apply, bump the version.
    void editTake(const std::function<void(take::Take&)>& edit);
    bool undoTake();
    bool canUndoTake() const { return takeHistory.canUndo(); }
    bool isAnalysingTake() const { return jobsRunning.load() > 0; }
    // Phase B: the take's original audio is held (false after a WAV that is missing), and the
    // corrected voice is being re-rendered in the background after an edit.
    bool hasTakeAudio() const { return takeAudio != nullptr; }
    bool isRenderingVoice() const { return renderBusy.load(); }
    // Blocks until no analysis or render is running or wanted, installing results as they land.
    // For tests and MCP. False on timeout.
    bool waitForTakeJobs(int timeoutMs);
    // The corrected voice as last installed (the original audio when no note needs rendering);
    // null without take audio. Message thread.
    std::shared_ptr<const std::vector<float>> getCorrectedAudio() const { return correctedAudio; }
    void clearTake(); // history cleared too; the WAV stays on disk

    // ---- Take Editor playback. Message thread. The take's notes replace the live MIDI while
    // playing, and the corrected take replaces the voice (the audio is untouched otherwise); the
    // engine still hears the live voice so the level bar works. Free play runs the
    // take's own clock; an anchored take also follows a playing host on its own. ----
    void playTake(double fromSeconds = 0.0); // no-op with no notes or while recording
    void stopTake();
    bool isTakePlaying() const { return player.isPlaying(); }
    double getTakePlayheadSeconds() const { return player.playheadSeconds(); }

    // Where takes are saved. Defaults to <AppData>/OK Studio/Murmur/Takes; tests redirect it.
    void setTakesFolder(const juce::File& folder) { takesFolder = folder; }
    juce::File getTakesFolder() const { return takesFolder; }

    juce::AudioProcessorValueTreeState apvts;

protected:
    // For Murmur Host: the same processor plus the host-only "mix" parameter.
    explicit MurmurProcessor(bool hostLayout);

    // Latency the subclass adds on top of the engine's (a hosted plugin's). Message thread
    // or while processing is suspended; the host is told the sum.
    void setExtraLatency(int samples);

    // The shared pieces of state, so a subclass saves and restores exactly what Murmur does.
    juce::ValueTree calibrationToTree() const;
    void restoreCalibration(const juce::ValueTree& root);
    juce::ValueTree takeToTree() const;
    void restoreTake(const juce::ValueTree& root);

private:
    struct AnalysisSetup
    {
        dsp::NoteSegmenter::Params params;
        float onsetSensitivity = 0.5f;
    };
    struct PendingResult
    {
        int generation = 0;
        bool curveOnly = false; // a restore: only `take.curve` is wanted
        take::Take take;
        juce::String error;
        std::shared_ptr<const std::vector<float>> audio; // the take's original mono audio, if known
    };
    struct RenderResult
    {
        int generation = 0;
        std::shared_ptr<const std::vector<float>> corrected;
        std::vector<take::VoiceRenderer::RenderedNote> snapshot; // the notes `corrected` reflects
    };

    AnalysisSetup makeAnalysisSetup() const;
    void startJob(std::function<PendingResult()> work);
    void bumpTakeVersion(); // also republishes the playback snapshot and schedules a voice render
    void scheduleVoiceRender();
    void resetVoice(); // no corrected audio any more; the voice is silent until a render lands
    void handleAsyncUpdate() override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout(bool hostLayout);
    void pushParametersToEngine();

    dsp::MurmurEngine engine;
    std::vector<float> mono;
    std::vector<dsp::MurmurEngine::MidiOut> midiScratch;
    juce::int64 samplePosition = 0;
    int lastMode = -1;
    int lastLatency = -1;
    std::atomic<int> extraLatency { 0 };
    std::atomic<bool> hostPlaying { false };
    std::atomic<double> hostBpm { 120.0 };
    std::unique_ptr<MurmurMcp> mcpBridge;

    // Take Editor. `take`, `takeHistory`, `takeError` belong to the message thread. Jobs only
    // touch their own copies plus `pending` (under pendingLock); ~MurmurProcessor stops them.
    take::TakeCapture capture;
    take::TakePlayer player;
    take::VoicePlayer voice;
    juce::MidiBuffer playbackMidi; // audio thread scratch, sized in prepareToPlay
    std::array<bool, 128> engineHeld {}; // engine notes sounding, so a takeover can release them
    take::Take take;
    // Phase B, message thread: the original audio, the corrected copy the audio thread plays
    // (through `voice`) and the notes it was rendered from. One render job at a time; edits
    // that land meanwhile just set `renderDirty`, and one more render follows with the latest.
    std::shared_ptr<const std::vector<float>> takeAudio, correctedAudio;
    std::vector<take::VoiceRenderer::RenderedNote> renderedSnapshot;
    bool renderDirty = false;
    std::atomic<bool> renderBusy { false };
    std::atomic<int> renderJobsRunning { 0 };
    take::TakeHistory takeHistory;
    juce::String takeError;
    juce::File takesFolder;
    std::atomic<int> takeVersion { 0 };
    std::atomic<int> takeGeneration { 0 }; // a result from an older generation is dropped
    std::atomic<int> jobsRunning { 0 };
    juce::CriticalSection pendingLock;
    std::optional<PendingResult> pending;
    std::optional<RenderResult> pendingRender;
    juce::ThreadPool takePool { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MurmurProcessor)
};
} // namespace murmur
