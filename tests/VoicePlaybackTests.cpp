// Take Editor, Phase B part 2 (docs/TAKE_EDITOR_DESIGN.md): the edited voice is audible. A take is
// recorded through a real processor, edited, re-rendered in the background and played back; the
// output is measured. tests/DspTests.cpp owns the runner's main(); this file is self-registering
// statics only.

#include "../src/PluginProcessor.h"
#include "../src/dsp/PitchTracker.h"
#if MURMUR_TESTS_HOST
#include "../src/host/MurmurHostProcessor.h"
#endif
#include "../src/take/VoiceRenderer.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace murmur::tests
{
namespace
{
    constexpr double kRate = 44100.0;
    constexpr int kBlock = 512;

    void report(const juce::String& line) { std::puts(("    " + line).toRawUTF8()); }

    // Three half-second notes (A3, C4, E4) with a 0.2 s lead-in, optionally with vibrato.
    std::vector<float> sungMelody(double vibratoSemis)
    {
        std::vector<float> audio((size_t) (2.4 * kRate), 0.0f);
        const double hz[3] = { 220.0, 261.63, 329.63 };
        for (int k = 0; k < 3; ++k)
        {
            const int a = (int) ((0.2 + 0.7 * k) * kRate), n = (int) (0.5 * kRate);
            double ph = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kRate;
                const double f = hz[k] * std::pow(2.0, vibratoSemis * std::sin(6.283185307 * 5.5 * t) / 12.0);
                ph += 6.283185307 * f / kRate;
                const float env = std::min({ 1.0f, (float) i / 400.0f, (float) (n - i) / 900.0f });
                audio[(size_t) (a + i)] = 0.4f * env * (float) (std::sin(ph) + 0.5 * std::sin(2 * ph));
            }
        }
        return audio;
    }

    // Fractional MIDI of clear voiced frames whose centre lies in [fromSec, toSec].
    std::vector<double> midiFrames(const std::vector<float>& v, double fromSec, double toSec)
    {
        dsp::PitchTracker pt;
        pt.prepare(kRate, 512);
        std::vector<double> m;
        dsp::PitchFrame fr;
        for (int pos = 0; pos < (int) v.size(); pos += 256) // popped as it goes: the queue holds 128 frames
        {
            pt.process(v.data() + pos, std::min(256, (int) v.size() - pos));
            while (pt.pop(fr))
            {
                const double t = (double) fr.samplePos / kRate;
                if (t >= fromSec && t <= toSec && fr.f0Hz > 0.0 && fr.clarity > 0.8f)
                    m.push_back(dsp::PitchTracker::hzToMidi(fr.f0Hz));
            }
        }
        return m;
    }

    double median(std::vector<double> v)
    {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    double stdDev(const std::vector<double>& v)
    {
        if (v.size() < 2)
            return 0.0;
        double mean = 0.0, s = 0.0;
        for (double x : v)
            mean += x;
        mean /= (double) v.size();
        for (double x : v)
            s += (x - mean) * (x - mean);
        return std::sqrt(s / (double) v.size());
    }

    float maxJump(const std::vector<float>& v)
    {
        float m = 0.0f;
        for (size_t i = 1; i < v.size(); ++i)
            m = std::max(m, std::abs(v[i] - v[i - 1]));
        return m;
    }

    float maxAbsDiff(const std::vector<float>& a, int aOff, const std::vector<float>& b, int bOff, int count)
    {
        float m = 0.0f;
        for (int i = 0; i < count; ++i)
            m = std::max(m, std::abs(a[(size_t) (aOff + i)] - b[(size_t) (bOff + i)]));
        return m;
    }

    int firstAbove(const std::vector<float>& v, int from, int to, float threshold)
    {
        for (int i = std::max(0, from); i < std::min(to, (int) v.size()); ++i)
            if (std::abs(v[(size_t) i]) > threshold)
                return i;
        return -1;
    }

    struct Out
    {
        std::vector<float> audio;          // channel 0
        std::vector<juce::int64> noteOns; // absolute sample within this run
    };

    // A processor (plain or Host) holding a take recorded from `audio` and analysed.
    template <class P>
    struct Rig
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        P processor;
        std::vector<float> audio;
        juce::File folder;

        explicit Rig(std::vector<float> sung, bool record = true) : audio(std::move(sung))
        {
            processor.setRateAndBufferSizeDetails(kRate, kBlock);
            processor.prepareToPlay(kRate, kBlock);
            folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("murmur-voice-test", "");
            processor.setTakesFolder(folder);
            if (record)
            {
                processor.startRecording();
                run(audio, (int) audio.size());
                processor.stopRecording();
                processor.waitForTakeAnalysis(20000);
                processor.waitForTakeJobs(20000);
            }
        }

        ~Rig() { folder.deleteRecursively(); }

        Out run(const std::vector<float>& in, int totalSamples)
        {
            Out o;
            juce::AudioBuffer<float> buf(2, kBlock);
            juce::MidiBuffer midi;
            for (int pos = 0; pos < totalSamples; pos += kBlock)
            {
                const int n = std::min(kBlock, totalSamples - pos);
                buf.setSize(2, n, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        buf.setSample(ch, i, pos + i < (int) in.size() ? in[(size_t) (pos + i)] : 0.0f);
                midi.clear();
                processor.processBlock(buf, midi);
                o.audio.insert(o.audio.end(), buf.getReadPointer(0), buf.getReadPointer(0) + n);
                for (const auto meta : midi)
                    if (meta.getMessage().isNoteOn())
                        o.noteOns.push_back(pos + meta.samplePosition);
            }
            return o;
        }

        int latency() const { return processor.getLatencySamples(); }
        int sampleOf(double seconds) const { return (int) std::llround(seconds * kRate); }
    };

    using PlainRig = Rig<MurmurProcessor>;
#if MURMUR_TESTS_HOST
    using HostRig = Rig<MurmurHostProcessor>;
#endif
} // namespace

class VoicePlaybackTests : public juce::UnitTest
{
public:
    VoicePlaybackTests() : juce::UnitTest("Voice playback", "murmur") {}

    void runTest() override
    {
        beginTest("the recording keeps its audio; a take exactly as sung needs no render");
        {
            PlainRig r(sungMelody(0.0));
            expect(r.processor.hasTakeAudio(), "audio kept");
            expect(! r.processor.isRenderingVoice(), "no render pending");
            expectEquals((int) r.processor.getTake().notes.size(), 3);
            const auto corrected = r.processor.getCorrectedAudio();
            expect(corrected != nullptr && corrected->size() == r.audio.size(), "the original is the voice");
            if (corrected != nullptr && corrected->size() == r.audio.size())
                expectEquals(maxAbsDiff(*corrected, 0, r.audio, 0, (int) r.audio.size()), 0.0f);
        }

        beginTest("free play outputs the edited voice: shifted inside the note, as sung outside");
        {
            PlainRig r(sungMelody(0.0));
            if (r.processor.getTake().notes.size() != 3)
            {
                expect(false, "three notes detected");
                return;
            }
            const auto before = r.processor.getTake();
            r.processor.editTake([](take::Take& t) { t.notes[1].targetNote += 3; });
            expect(r.processor.isRenderingVoice(), "the edit starts a render");
            expect(r.processor.waitForTakeJobs(20000), "the render finishes");
            expect(! r.processor.isRenderingVoice());

            r.processor.playTake(0.0);
            const int total = (int) r.audio.size() + 4096;
            const auto out = r.run({}, total); // silence in: whatever is heard is the take
            const int lat = r.latency();
            const auto& n1 = r.processor.getTake().notes[1];

            // The output sample for take sample i is out[i + lat]. Measure on the output itself.
            const double a = n1.start + 0.1, b = n1.end - 0.1;
            const double midi = median(midiFrames(out.audio, a + lat / kRate, b + lat / kRate));
            const double expected = dsp::PitchTracker::hzToMidi(261.63) + 3.0;
            report("edited note: sung " + juce::String(dsp::PitchTracker::hzToMidi(261.63), 2) + ", output "
                   + juce::String(midi, 2) + " MIDI (expected " + juce::String(expected, 2) + ")");
            expectWithinAbsoluteError(midi, expected, 0.3);

            // Untouched notes are the recording, sample for sample, at the latency offset.
            const auto n0 = before.notes[0], n2 = before.notes[2];
            for (const auto& n : { n0, n2 })
            {
                const int s = r.sampleOf(n.start), len = r.sampleOf(n.end - n.start);
                const float d = maxAbsDiff(out.audio, s + lat, r.audio, s, len);
                report("untouched note at " + juce::String(n.start, 2) + " s: max difference " + juce::String(d, 6));
                expectLessThan(d, 1.0e-6f);
            }

            // Aligned with the MIDI: the voice starts where the recording does, shifted by the
            // reported latency, the same as the note-on is.
            const int onsetOrig = firstAbove(r.audio, r.sampleOf(n1.start - 0.1), r.sampleOf(n1.start + 0.2), 0.02f);
            const int onsetOut = firstAbove(out.audio, r.sampleOf(n1.start - 0.1) + lat, r.sampleOf(n1.start + 0.2) + lat, 0.02f);
            expect(onsetOrig >= 0 && onsetOut >= 0, "both have an onset");
            const int drift = onsetOut - (onsetOrig + lat);
            report("voice onset vs recording + latency: " + juce::String(drift) + " samples ("
                   + juce::String(1000.0 * drift / kRate, 2) + " ms), latency " + juce::String(lat));
            expectLessThan(std::abs(drift), (int) (0.003 * kRate));
            const juce::int64 noteOn = r.sampleOf(n1.start) + lat;
            expect(std::find(out.noteOns.begin(), out.noteOns.end(), noteOn) != out.noteOns.end(),
                   "the MIDI note-on sits at the same take time plus latency");
        }

        beginTest("several quick edits end with the render of the final take");
        {
            PlainRig r(sungMelody(0.0));
            if (r.processor.getTake().notes.size() != 3)
            {
                expect(false, "three notes detected");
                return;
            }
            for (int k = 1; k <= 5; ++k)
                r.processor.editTake([k](take::Take& t)
                {
                    t.notes[0].targetNote = t.notes[0].detectedNote + k;
                    t.notes[1].drift = 1.0f - 0.15f * (float) k;
                    if (k == 4)
                        t.notes[2].targetNote = t.notes[2].detectedNote - 2;
                });
            expect(r.processor.isRenderingVoice(), "a render is running or wanted");
            expect(r.processor.waitForTakeJobs(30000), "everything settles");
            expect(! r.processor.isRenderingVoice());

            const auto expected = take::VoiceRenderer::renderAll(r.audio, r.processor.getTake(), kRate);
            const auto got = r.processor.getCorrectedAudio();
            expect(got != nullptr && got->size() == expected.size(), "a corrected buffer");
            if (got != nullptr && got->size() == expected.size())
            {
                const float d = maxAbsDiff(*got, 0, expected, 0, (int) expected.size());
                report("incremental vs renderAll of the final take: max difference " + juce::String(d, 8));
                expectLessThan(d, 1.0e-6f);
            }

            // Undo and erase bring the original back over the edited regions.
            r.processor.editTake([](take::Take& t) { take::deleteNote(t, 0); });
            r.processor.waitForTakeJobs(30000);
            const auto afterErase = take::VoiceRenderer::renderAll(r.audio, r.processor.getTake(), kRate);
            const auto got2 = r.processor.getCorrectedAudio();
            expect(got2 != nullptr && got2->size() == afterErase.size());
            if (got2 != nullptr && got2->size() == afterErase.size())
                expectLessThan(maxAbsDiff(*got2, 0, afterErase, 0, (int) afterErase.size()), 1.0e-6f);
        }

        beginTest("not playing: the output is the untouched input; and again after the take stops");
        {
            PlainRig r(sungMelody(0.0));
            r.processor.editTake([](take::Take& t) { t.notes[1].targetNote += 4; });
            r.processor.waitForTakeJobs(20000);

            std::vector<float> noise((size_t) (1.0 * kRate));
            juce::Random rng(42);
            for (auto& x : noise)
                x = rng.nextFloat() * 0.5f - 0.25f;
            const auto idle = r.run(noise, (int) noise.size());
            expectEquals(maxAbsDiff(idle.audio, 0, noise, 0, (int) noise.size()), 0.0f);

            r.processor.playTake(0.0);
            r.run({}, 8 * kBlock);
            r.processor.stopTake();
            r.run({}, 4 * kBlock); // the fade-out
            const auto after = r.run(noise, (int) noise.size());
            expectEquals(maxAbsDiff(after.audio, 0, noise, 0, (int) noise.size()), 0.0f);
        }

        beginTest("starting and stopping mid-note do not click");
        {
            PlainRig r(sungMelody(0.0));
            r.processor.editTake([](take::Take& t) { t.notes[0].targetNote += 2; });
            r.processor.waitForTakeJobs(20000);
            const auto& n0 = r.processor.getTake().notes[0];

            std::vector<float> noise((size_t) (1.0 * kRate));
            juce::Random rng(7);
            for (auto& x : noise)
                x = rng.nextFloat() * 0.5f - 0.25f;

            r.run(noise, 2 * kBlock);
            r.processor.playTake(n0.start + 0.15); // into the middle of a sustained note
            const auto played = r.run({}, 12 * kBlock);
            r.processor.stopTake();
            const auto stopped = r.run({}, 6 * kBlock);

            // Silence in, so the step from silence to the voice is the only thing that could click.
            // A tone of this level moves at most ~0.04 per sample; an unramped start would jump
            // by the note's whole level.
            std::vector<float> all = played.audio;
            all.insert(all.end(), stopped.audio.begin(), stopped.audio.end());
            const float jump = maxJump(all);
            report("largest sample step across start and stop: " + juce::String(jump, 4));
            expectLessThan(jump, 0.1f);
            expect(std::abs(stopped.audio.back()) < 1.0e-6f, "and it ends silent");
        }

        beginTest("drift 0 flattens the vibrato of the rendered voice");
        {
            PlainRig r(sungMelody(0.4));
            if (r.processor.getTake().notes.size() != 3)
            {
                expect(false, "three notes detected");
                return;
            }
            const auto n1 = r.processor.getTake().notes[1];
            const double a = n1.start + 0.08, b = n1.end - 0.08;
            const double before = stdDev(midiFrames(r.audio, a, b));

            r.processor.editTake([](take::Take& t) { t.notes[1].drift = 0.0f; });
            r.processor.waitForTakeJobs(20000);
            const auto got = r.processor.getCorrectedAudio();
            expect(got != nullptr && got->size() == r.audio.size(), "a corrected buffer");
            if (got == nullptr || got->size() != r.audio.size())
                return;
            const auto frames = midiFrames(*got, a, b);
            const double after = stdDev(frames);
            report("pitch spread inside the note: " + juce::String(before, 3) + " semitones sung, "
                   + juce::String(after, 3) + " at drift 0 (centre " + juce::String(median(frames), 2) + ")");
            expect(! frames.empty(), "pitch found");
            expectLessThan(after, before * 0.5);
            expectWithinAbsoluteError(median(frames), (double) n1.targetNote, 0.2);

            // Half drift sits between.
            r.processor.editTake([](take::Take& t) { t.notes[1].drift = 0.5f; });
            r.processor.waitForTakeJobs(20000);
            const double half = stdDev(midiFrames(*r.processor.getCorrectedAudio(), a, b));
            report("pitch spread at drift 0.5: " + juce::String(half, 3));
            expect(half > after && half < before, "drift 0.5 is between flat and as sung");
        }

#if MURMUR_TESTS_HOST
        beginTest("Murmur Host with no instrument: the corrected take is the output while playing");
        {
            HostRig r(sungMelody(0.0));
            if (r.processor.getTake().notes.size() != 3)
            {
                expect(false, "three notes detected");
                return;
            }
            r.processor.editTake([](take::Take& t) { t.notes[2].targetNote -= 3; });
            r.processor.waitForTakeJobs(20000);

            std::vector<float> noise((size_t) (0.5 * kRate));
            juce::Random rng(3);
            for (auto& x : noise)
                x = rng.nextFloat() * 0.5f - 0.25f;
            const auto idle = r.run(noise, (int) noise.size());
            expectEquals(maxAbsDiff(idle.audio, 0, noise, 0, (int) noise.size()), 0.0f);

            r.processor.playTake(0.0);
            const auto out = r.run(noise, (int) r.audio.size() + 4096); // live voice in: it must not be heard
            const auto& n2 = r.processor.getTake().notes[2];
            const int lat = r.latency();
            const double midi = median(midiFrames(out.audio, n2.start + 0.1 + lat / kRate, n2.end - 0.1 + lat / kRate));
            const double expected = dsp::PitchTracker::hzToMidi(329.63) - 3.0;
            report("host, edited note: output " + juce::String(midi, 2) + " MIDI (expected "
                   + juce::String(expected, 2) + ")");
            expectWithinAbsoluteError(midi, expected, 0.3);
            const int s = r.sampleOf(r.processor.getTake().notes[0].start), len = r.sampleOf(0.4);
            expectLessThan(maxAbsDiff(out.audio, s + lat, r.audio, s, len), 1.0e-6f);
        }
#endif

        beginTest("a restored take reloads its audio; a missing WAV leaves MIDI only");
        {
            PlainRig r(sungMelody(0.0));
            r.processor.editTake([](take::Take& t) { t.notes[1].targetNote += 2; });
            r.processor.waitForTakeJobs(20000);
            juce::MemoryBlock state;
            r.processor.getStateInformation(state);

            PlainRig again({}, false);
            again.processor.setStateInformation(state.getData(), (int) state.getSize());
            expect(again.processor.waitForTakeJobs(20000));
            expect(again.processor.hasTakeAudio(), "the WAV was read back");
            const auto corrected = again.processor.getCorrectedAudio();
            const auto expected = take::VoiceRenderer::renderAll(r.audio, again.processor.getTake(), kRate);
            expect(corrected != nullptr && corrected->size() == expected.size(), "the edit was rendered");
            if (corrected != nullptr && corrected->size() == expected.size())
                expectLessThan(maxAbsDiff(*corrected, 0, expected, 0, (int) expected.size()), 1.0e-6f);

            juce::File(r.processor.getTake().audioPath).deleteFile();
            PlainRig gone({}, false);
            gone.processor.setStateInformation(state.getData(), (int) state.getSize());
            gone.processor.waitForTakeJobs(20000);
            expect(! gone.processor.hasTakeAudio(), "no audio without the WAV");
            expectEquals((int) gone.processor.getTake().notes.size(), 3);
            expect(! gone.processor.isRenderingVoice());

            gone.processor.playTake(0.0);
            std::vector<float> noise((size_t) (1.0 * kRate), 0.2f);
            const auto out = gone.run(noise, (int) noise.size());
            float peak = 0.0f;
            for (size_t i = 1000; i < out.audio.size(); ++i)
                peak = std::max(peak, std::abs(out.audio[i]));
            expectLessThan(peak, 1.0e-6f); // the voice is silent, and the live input is not heard
            expect(! out.noteOns.empty(), "but the MIDI plays");
        }
    }
};

static VoicePlaybackTests voicePlaybackTests;
} // namespace murmur::tests
