// Take Editor playback (docs/TAKE_EDITOR_DESIGN.md, step 4): a take's notes become MIDI through
// a real MurmurProcessor, free-running or following a test AudioPlayHead. tests/DspTests.cpp
// owns the runner's main(); this file is self-registering statics only.

#include "../src/PluginProcessor.h"
#include "../src/take/TakePlayer.h"

#include <juce_events/juce_events.h>

#include <cmath>
#include <functional>

namespace murmur::tests
{
namespace
{
    constexpr double kRate = 44100.0;
    constexpr int kBlock = 512;

    struct Ev
    {
        juce::int64 at = 0; // absolute sample since the first block
        bool on = false;
        int note = 0;
        int velocity = 0;
    };

    struct TestHead : juce::AudioPlayHead
    {
        bool playing = false;
        double ppq = 0.0, bpm = 120.0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setIsPlaying(playing);
            p.setPpqPosition(ppq);
            p.setBpm(bpm);
            return p;
        }
    };

    // A processor with a known take installed (three notes of half a second at C4, E4, G4).
    struct Rig
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        MurmurProcessor processor;
        TestHead head;
        juce::int64 blocksRun = 0;

        explicit Rig(bool withTake = true, std::optional<double> anchor = std::nullopt)
        {
            processor.setRateAndBufferSizeDetails(kRate, kBlock);
            processor.prepareToPlay(kRate, kBlock);
            processor.setPlayHead(&head);
            if (withTake)
                processor.editTake([anchor](take::Take& t)
                {
                    t.sampleRate = kRate;
                    t.anchorPpq = anchor;
                    t.anchorBpm = 120.0;
                    t.notes.clear();
                    const int notes[] = { 60, 64, 67 };
                    for (int i = 0; i < 3; ++i)
                    {
                        take::TakeNote n;
                        n.start = 0.5 + 1.0 * i;
                        n.end = n.start + 0.5;
                        n.sungMidi = (float) notes[i];
                        n.detectedNote = n.targetNote = notes[i];
                        n.velocity = 0.5f;
                        t.notes.push_back(n);
                    }
                });
        }

        juce::int64 latency() const { return processor.getLatencySamples(); }

        // Runs silent blocks; `before(blockIndex)` may move the test head or poke the processor.
        std::vector<Ev> run(int blocks, const std::function<void(int)>& before = {})
        {
            std::vector<Ev> out;
            juce::AudioBuffer<float> buf(2, kBlock);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b, ++blocksRun)
            {
                if (before)
                    before(b);
                buf.clear();
                midi.clear();
                processor.processBlock(buf, midi);
                for (const auto meta : midi)
                {
                    const auto m = meta.getMessage();
                    if (m.isNoteOn() || m.isNoteOff())
                        out.push_back({ blocksRun * kBlock + meta.samplePosition, m.isNoteOn(), m.getNoteNumber(),
                                        m.isNoteOn() ? (int) m.getVelocity() : 0 });
                }
            }
            return out;
        }
    };

    juce::int64 sampleOf(double seconds) { return (juce::int64) std::llround(seconds * kRate); }
} // namespace

class TakePlaybackTests : public juce::UnitTest
{
public:
    TakePlaybackTests() : juce::UnitTest("Take playback", "murmur") {}

    // Checks the six events of the known take against their expected absolute sample times.
    void expectKnownTake(const std::vector<Ev>& ev, juce::int64 latency, const int (&notes)[3])
    {
        expectEquals((int) ev.size(), 6, "three note-ons and three note-offs");
        if (ev.size() != 6)
            return;
        for (int i = 0; i < 3; ++i)
        {
            const auto& on = ev[(size_t) i * 2];
            const auto& off = ev[(size_t) i * 2 + 1];
            expect(on.on && ! off.on, "on then off");
            expectEquals(on.note, notes[i]);
            expectEquals(off.note, notes[i]);
            expectEquals((int) on.velocity, 64);
            expectEquals((int) on.at, (int) (sampleOf(0.5 + 1.0 * i) + latency));
            expectEquals((int) off.at, (int) (sampleOf(1.0 + 1.0 * i) + latency));
        }
    }

    void runTest() override
    {
        const int cMajor[3] = { 60, 64, 67 };

        beginTest("free play emits the take's notes at the right samples, then stops itself");
        {
            Rig r;
            expect(! r.processor.isTakePlaying(), "idle at first");
            r.processor.playTake(0.0);
            expect(r.processor.isTakePlaying(), "playing");
            const auto ev = r.run(300); // 3 s of take plus the latency
            expectKnownTake(ev, r.latency(), cMajor);
            expect(! r.processor.isTakePlaying(), "stopped at the end of the take");
            expect(r.run(20).empty(), "silent afterwards");
        }

        beginTest("playing from the middle skips notes that already started");
        {
            Rig r;
            r.processor.playTake(1.2);
            const auto ev = r.run(300);
            expectEquals((int) ev.size(), 4, "two notes left");
            if (ev.size() == 4)
                expectEquals(ev[0].note, 64);
        }

        beginTest("an edited target note is the note that plays");
        {
            Rig r;
            r.processor.editTake([](take::Take& t) { t.notes[1].targetNote = 65; });
            r.processor.playTake(0.0);
            const auto ev = r.run(300);
            const int expected[3] = { 60, 65, 67 };
            expectKnownTake(ev, r.latency(), expected);
        }

        beginTest("an edit while playing silences the sounding note and keeps going");
        {
            Rig r;
            r.processor.playTake(0.0);
            // 0.7 s: the first note is sounding.
            auto ev = r.run(61);
            expectEquals((int) ev.size(), 1, "note-on only so far");
            r.processor.editTake([](take::Take& t) { t.notes[2].targetNote = 69; });
            const auto rest = r.run(260);
            expect(! rest.empty() && ! rest.front().on && rest.front().note == 60, "released at once");
            expect(rest.back().note == 67 || rest.back().note == 69, "the last note still plays");
            bool sawNew = false;
            for (const auto& e : rest)
                sawNew = sawNew || (e.on && e.note == 69);
            expect(sawNew, "and plays as its new target");
        }

        beginTest("stopTake sends a note-off for the sounding note and then nothing");
        {
            Rig r;
            r.processor.playTake(0.0);
            auto ev = r.run(61);
            expectEquals((int) ev.size(), 1, "sounding");
            r.processor.stopTake();
            expect(! r.processor.isTakePlaying(), "stopped");
            const auto after = r.run(200);
            expectEquals((int) after.size(), 1, "just the release");
            if (after.size() == 1)
            {
                expect(! after[0].on && after[0].note == 60, "note-off of the sounding note");
                expectEquals((int) after[0].at, (int) (61 * kBlock));
            }
        }

        beginTest("nothing plays while recording, and recording stops playback");
        {
            Rig r;
            expect(r.processor.startRecording(), "recording");
            r.processor.playTake(0.0);
            expect(! r.processor.isTakePlaying(), "play refused while recording");
            expect(r.run(100).empty(), "silent");
            r.processor.getCapture().stop();

            Rig r2;
            r2.processor.playTake(0.0);
            r2.run(61);
            r2.processor.startRecording();
            expect(! r2.processor.isTakePlaying(), "recording stops playback");
            const auto ev = r2.run(10);
            expectEquals((int) ev.size(), 1, "the sounding note is released");
            r2.processor.getCapture().stop();
        }

        beginTest("follow host: an anchored take plays in place under a playing host");
        {
            Rig r(true, 8.0);
            r.head.playing = true;
            const auto ev = r.run(300, [&r](int b)
            {
                r.head.ppq = 8.0 + (double) (b * kBlock) / kRate * 2.0; // 120 bpm: 2 beats a second
            });
            expectKnownTake(ev, r.latency(), cMajor);
            expect(! r.processor.isTakePlaying(), "not playing once the host has run past the take");
        }

        beginTest("follow host: silent outside the take, when the host is stopped, and for free takes");
        {
            Rig r(true, 8.0);
            r.head.playing = true;
            r.head.ppq = 100.0;
            expect(r.run(100).empty(), "host far past the take");
            expect(! r.processor.isTakePlaying(), "not playing");

            r.head.ppq = 4.0;
            expect(r.run(100, [&r](int b) { r.head.ppq = 4.0 + (double) (b * kBlock) / kRate * 2.0; }).empty(),
                   "host before the take");

            r.head.playing = false;
            r.head.ppq = 9.0;
            expect(r.run(60).empty(), "host stopped");

            Rig free;
            free.head.playing = true;
            expect(free.run(100, [&free](int b) { free.head.ppq = (double) (b * kBlock) / kRate * 2.0; }).empty(),
                   "a free take never follows the host");
        }

        beginTest("follow host: a seek releases the sounding note");
        {
            Rig r(true, 8.0);
            r.head.playing = true;
            const auto ev = r.run(161, [&r](int b)
            {
                // 0.7 s in (first note sounding) at block 61, then the host jumps back to the start.
                const int pos = b < 61 ? b : b - 61;
                r.head.ppq = 8.0 + (double) (pos * kBlock) / kRate * 2.0;
            });
            int ons = 0, offs = 0;
            for (const auto& e : ev)
                (e.on ? ons : offs)++;
            expectEquals(ons, 2, "the first note plays again after the jump");
            expectEquals(offs, 2, "and was released at the jump and at its end");
        }

        beginTest("free play wins over the host");
        {
            Rig r(true, 8.0);
            r.head.playing = true;
            r.processor.playTake(2.2);
            const auto ev = r.run(100, [&r](int b)
            {
                r.head.ppq = 8.0 + (double) (b * kBlock) / kRate * 2.0;
            });
            expectEquals((int) ev.size(), 2, "only the last note, from the free clock");
            if (! ev.empty())
                expectEquals(ev[0].note, 67);
        }

        beginTest("idle player leaves the live engine's MIDI alone; playing replaces it");
        {
            // A real take from sung audio, then the same audio sung again.
            std::vector<float> audio((size_t) (2.4 * kRate), 0.0f);
            const double hz[3] = { 220.0, 261.63, 329.63 };
            for (int k = 0; k < 3; ++k)
            {
                const int a = (int) ((0.2 + 0.7 * k) * kRate), n = (int) (0.5 * kRate);
                for (int i = 0; i < n; ++i)
                {
                    const float env = std::min({ 1.0f, (float) i / 400.0f, (float) (n - i) / 900.0f });
                    const double ph = 6.283185307 * hz[k] * (double) i / kRate;
                    audio[(size_t) (a + i)] = 0.4f * env * (float) (std::sin(ph) + 0.5 * std::sin(2 * ph));
                }
            }

            auto sing = [&audio](Rig& r)
            {
                std::vector<Ev> out;
                juce::AudioBuffer<float> buf(2, kBlock);
                juce::MidiBuffer midi;
                for (size_t pos = 0, b = 0; pos < audio.size(); pos += kBlock, ++b)
                {
                    const int n = (int) std::min<size_t>(kBlock, audio.size() - pos);
                    buf.setSize(2, n, false, false, true);
                    for (int ch = 0; ch < 2; ++ch)
                        std::copy_n(audio.data() + pos, n, buf.getWritePointer(ch));
                    midi.clear();
                    r.processor.processBlock(buf, midi);
                    for (const auto meta : midi)
                        if (meta.getMessage().isNoteOn())
                            out.push_back({ (juce::int64) (b * kBlock) + meta.samplePosition, true,
                                            meta.getMessage().getNoteNumber(), 0 });
                }
                return out;
            };

            Rig r(false);
            const auto live = sing(r);
            expect(live.size() >= 3, "the engine hears the three notes live");

            r.processor.setTakesFolder(juce::File::getSpecialLocation(juce::File::tempDirectory)
                                           .getNonexistentChildFile("murmur-playback-test", ""));
            r.processor.startRecording();
            sing(r);
            r.processor.stopRecording();
            r.processor.waitForTakeAnalysis(20000);
            const auto folder = r.processor.getTakesFolder();
            const auto& t = r.processor.getTake();
            expectEquals((int) t.notes.size(), 3);

            r.processor.editTake([](take::Take& tk) { tk.notes[0].targetNote += 5; });
            r.processor.playTake(0.0);
            const auto played = sing(r);
            expectEquals((int) played.size(), 3, "only the take's notes, not the live ones");
            if (played.size() == 3)
            {
                expectEquals(played[0].note, t.notes[0].targetNote);
                expectEquals((int) played[0].at, (int) (sampleOf(t.notes[0].start) + r.latency()));
            }
            if (t.audioPath.isNotEmpty())
                juce::File(t.audioPath).deleteFile();
            folder.deleteRecursively();
        }

        beginTest("the snapshot caps at 2048 notes and an empty publish plays nothing");
        {
            take::TakePlayer p;
            take::Take big;
            for (int i = 0; i < 3000; ++i)
            {
                take::TakeNote n;
                n.start = 0.001 * i;
                n.end = n.start + 0.0005;
                big.notes.push_back(n);
            }
            p.publish(big, kRate);
            p.play(0.0);
            juce::MidiBuffer out;
            int ons = 0;
            for (int b = 0; b < 200; ++b)
            {
                out.clear();
                p.render(out, kBlock, 0, false, 0.0, kRate);
                for (const auto meta : out)
                    ons += meta.getMessage().isNoteOn() ? 1 : 0;
            }
            expectEquals(ons, take::TakePlayer::maxNotes);

            p.publish(take::Take {}, kRate);
            p.play(0.0);
            out.clear();
            p.render(out, kBlock, 0, false, 0.0, kRate);
            expect(out.isEmpty(), "nothing to play");
            expect(! p.isPlaying(), "and it ends at once");
        }
    }
};

static TakePlaybackTests takePlaybackTests;
} // namespace murmur::tests
