// Take Editor capture (docs/TAKE_EDITOR_DESIGN.md, step 2): the lock-free buffer on its own,
// then Record and Stop through a real MurmurProcessor, the WAV, and the take in plugin state.
// tests/DspTests.cpp owns the runner's main(); this file is self-registering statics only.

#include "../src/PluginProcessor.h"
#include "../src/take/TakeCapture.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <cmath>

namespace murmur::tests
{
namespace
{
    constexpr double kRate = 44100.0;
    constexpr int kBlock = 512;

    // A voiced tone with a soft attack and release, placed in a silent buffer.
    void addBurst(std::vector<float>& v, double hz, double startSec, double lenSec)
    {
        const int a = (int) (startSec * kRate), n = (int) (lenSec * kRate);
        const int ramp = (int) (0.02 * kRate), atk = (int) (0.008 * kRate);
        for (int i = 0; i < n && a + i < (int) v.size(); ++i)
        {
            float env = 1.0f;
            if (i < atk) env = 0.5f - 0.5f * (float) std::cos(3.14159265 * i / atk);
            if (i > n - ramp) env = 0.5f - 0.5f * (float) std::cos(3.14159265 * (n - i) / ramp);
            const double ph = 6.283185307 * hz * (double) i / kRate;
            v[(size_t) (a + i)] += 0.4f * env * (float) (std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.25 * std::sin(3 * ph));
        }
    }

    double midiToHz(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

    std::vector<float> melody()
    {
        std::vector<float> audio((size_t) (2.4 * kRate), 0.0f);
        const int midis[] = { 57, 60, 64 }; // A3, C4, E4: all in C major
        for (int i = 0; i < 3; ++i)
            addBurst(audio, midiToHz(midis[i]), 0.2 + 0.7 * i, 0.5);
        return audio;
    }

    // A processor in its own takes folder, prepared like a host would.
    struct Host
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        MurmurProcessor processor;
        juce::File folder;

        Host()
        {
            folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("murmur-take-test", "");
            processor.setTakesFolder(folder);
            processor.setRateAndBufferSizeDetails(kRate, kBlock);
            processor.prepareToPlay(kRate, kBlock);
        }
        ~Host() { folder.deleteRecursively(); }

        // Feeds `audio` through processBlock in 512-sample blocks (mono voice on both channels).
        // Returns true if every block came out identical to what went in.
        bool feed(const std::vector<float>& audio)
        {
            bool untouched = true;
            juce::AudioBuffer<float> buf(2, kBlock);
            juce::MidiBuffer midi;
            for (size_t pos = 0; pos < audio.size(); pos += kBlock)
            {
                const int n = (int) std::min<size_t>(kBlock, audio.size() - pos);
                buf.setSize(2, n, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                    std::copy_n(audio.data() + pos, n, buf.getWritePointer(ch));
                processor.processBlock(buf, midi);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        untouched = untouched && buf.getReadPointer(ch)[i] == audio[pos + (size_t) i];
            }
            return untouched;
        }

        void record(const std::vector<float>& audio, bool* untouched = nullptr)
        {
            processor.startRecording();
            const bool same = feed(audio);
            if (untouched != nullptr)
                *untouched = same;
            processor.stopRecording();
            processor.waitForTakeAnalysis(20000);
        }
    };

    juce::MemoryBlock save(MurmurProcessor& p)
    {
        juce::MemoryBlock block;
        p.getStateInformation(block);
        return block;
    }
} // namespace

class TakeCaptureTests : public juce::UnitTest
{
public:
    TakeCaptureTests() : juce::UnitTest("Take capture", "murmur") {}

    void runTest() override
    {
        beginTest("capture returns exactly what was written, in order");
        {
            take::TakeCapture c;
            expect(! c.isRecording(), "idle at first");
            expect(c.arm(48000.0), "arms");
            expect(c.isRecording(), "recording");
            expect(! c.arm(48000.0), "a second arm is refused");
            std::vector<float> src(1000);
            for (size_t i = 0; i < src.size(); ++i)
                src[i] = (float) i;
            c.write(src.data(), 300, false, 0.0, 120.0);
            c.write(src.data() + 300, 700, false, 0.0, 120.0);
            expectWithinAbsoluteError(c.secondsRecorded(), 1000.0 / 48000.0, 1e-9);
            const auto r = c.stop();
            expect(! c.isRecording(), "stopped");
            expectEquals((int) r.audio.size(), 1000);
            expect(r.audio == src, "same samples");
            expectEquals(r.sampleRate, 48000.0);
            expect(! r.hitCap, "cap not reached");
            expect(! r.anchorPpq.has_value(), "no anchor when the host was stopped");
            expect(c.stop().audio.empty(), "stopping twice is harmless");
        }

        beginTest("the anchor is taken from the first write, and only if the host is playing");
        {
            take::TakeCapture c;
            const float s[4] = {};
            c.arm(44100.0);
            c.write(s, 4, true, 8.5, 90.0);
            c.write(s, 4, true, 99.0, 100.0); // later writes never move it
            auto r = c.stop();
            expect(r.anchorPpq.has_value(), "anchored");
            expectEquals(*r.anchorPpq, 8.5);
            expectEquals(r.anchorBpm, 90.0);

            c.arm(44100.0);
            c.write(s, 4, false, 1.0, 120.0);
            c.write(s, 4, true, 2.0, 120.0); // started while stopped: stays free
            expect(! c.stop().anchorPpq.has_value(), "free take");
        }

        beginTest("at the cap it stops filling and says so");
        {
            take::TakeCapture c;
            c.arm(1000.0, 0.01); // 10 samples
            std::vector<float> src(25, 1.0f);
            c.write(src.data(), 8, false, 0.0, 120.0);
            expect(! c.capReached(), "not yet");
            c.write(src.data(), 8, false, 0.0, 120.0);
            expect(c.capReached(), "cap reached");
            c.write(src.data(), 9, false, 0.0, 120.0);
            const auto r = c.stop();
            expectEquals((int) r.audio.size(), 10);
            expect(r.hitCap, "reported");
        }

        beginTest("processor: record a melody, stop, get 3 notes and a WAV");
        {
            Host h;
            expect(h.processor.startRecording(), "starts once prepared");
            expect(h.processor.getCapture().isRecording(), "recording");
            h.feed(melody());
            expect(h.processor.getCapture().secondsRecorded() > 2.3, "seconds counted");
            const int v0 = h.processor.getTakeVersion();
            h.processor.stopRecording();
            expect(h.processor.waitForTakeAnalysis(20000), "job finished");
            const auto& t = h.processor.getTake();
            expectEquals((int) t.notes.size(), 3);
            expect(h.processor.getTakeVersion() > v0, "version bumped");
            expect(! t.curve.empty(), "curve present");
            expect(! t.anchorPpq.has_value(), "free take (no playhead)");
            expect(t.audioPath.isNotEmpty() && juce::File(t.audioPath).existsAsFile(), "WAV saved");
            expect(juce::File(t.audioPath).isAChildOf(h.folder), "in the takes folder");
            expectEquals(h.processor.getTakeError(), juce::String());

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> reader(
                wav.createReaderFor(juce::File(t.audioPath).createInputStream().release(), true));
            expect(reader != nullptr, "WAV opens");
            if (reader != nullptr)
            {
                expectEquals((int) reader->numChannels, 1);
                expectEquals((int) reader->bitsPerSample, 32);
                expect(reader->usesFloatingPointData, "float");
                expectEquals(reader->sampleRate, kRate);
            }
            reader.reset();
            juce::File(t.audioPath).deleteFile();
        }

        beginTest("audio passes through untouched while recording");
        {
            Host h;
            bool untouched = false;
            h.record(melody(), &untouched);
            expect(untouched, "every block identical to its input");
        }

        beginTest("editTake and undoTake");
        {
            Host h;
            h.record(melody());
            const int original = h.processor.getTake().notes.at(1).targetNote;
            const int v0 = h.processor.getTakeVersion();
            h.processor.editTake([](take::Take& t) { t.notes[1].targetNote += 2; });
            expectEquals(h.processor.getTake().notes[1].targetNote, original + 2);
            expect(h.processor.getTakeVersion() > v0, "edit bumps the version");
            expect(h.processor.undoTake(), "undo works");
            expectEquals(h.processor.getTake().notes[1].targetNote, original);
            expect(! h.processor.undoTake(), "nothing more to undo");

            h.processor.clearTake();
            expect(h.processor.getTake().isEmpty(), "cleared");
            expect(! h.processor.undoTake(), "history cleared too");
        }

        beginTest("state keeps notes and edits, and rebuilds the curve from the WAV");
        {
            Host a;
            a.record(melody());
            a.processor.editTake([](take::Take& t) { t.notes[1].targetNote = 62; });
            const auto block = save(a.processor);

            Host b;
            b.processor.setStateInformation(block.getData(), (int) block.getSize());
            b.processor.waitForTakeAnalysis(20000);
            const auto& t = b.processor.getTake();
            expectEquals((int) t.notes.size(), 3);
            expectEquals(t.notes[1].targetNote, 62);
            expectEquals(t.notes[1].detectedNote, a.processor.getTake().notes[1].detectedNote);
            expect(! t.curve.empty(), "curve rebuilt");
            expectEquals(b.processor.getTakeError(), juce::String());
            expect(! b.processor.undoTake(), "restore starts with no history");

            // Missing audio: notes survive, the error says where it was.
            const auto path = a.processor.getTake().audioPath;
            juce::File(path).deleteFile();
            Host c;
            c.processor.setStateInformation(block.getData(), (int) block.getSize());
            c.processor.waitForTakeAnalysis(20000);
            expectEquals((int) c.processor.getTake().notes.size(), 3);
            expectEquals(c.processor.getTake().notes[1].targetNote, 62);
            expectEquals(c.processor.getTakeError(), "Take audio not found: " + path);
        }

        beginTest("an empty take saves nothing and restores to empty");
        {
            Host a;
            const auto block = save(a.processor);
            Host b;
            b.record(melody());
            expect(! b.processor.getTake().isEmpty(), "has a take first");
            b.processor.setStateInformation(block.getData(), (int) block.getSize());
            expect(b.processor.getTake().isEmpty(), "restored to empty");
        }
    }
};

static TakeCaptureTests takeCaptureTests;
} // namespace murmur::tests
