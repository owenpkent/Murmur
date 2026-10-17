// Take Editor model and offline analysis (docs/TAKE_EDITOR_DESIGN.md, step 1). Pure code, no
// processor needed. tests/DspTests.cpp owns the runner's main(); this file is self-registering
// statics only.

#include "../src/take/TakeAnalyser.h"
#include "../src/take/TakeModel.h"

#include <cmath>

namespace murmur::tests
{
namespace
{
    constexpr double kRate = 44100.0;

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

    take::Take takeWith(std::initializer_list<int> notes)
    {
        take::Take t;
        double s = 0.0;
        for (const int n : notes)
        {
            take::TakeNote tn;
            tn.start = s;
            tn.end = s + 0.5;
            tn.detectedNote = tn.targetNote = n;
            tn.sungMidi = (float) n;
            t.notes.push_back(tn);
            s += 0.6;
        }
        return t;
    }
} // namespace

class TakeTests : public juce::UnitTest
{
public:
    TakeTests() : juce::UnitTest("Take model and analyser", "murmur") {}

    void runTest() override
    {
        constexpr int cMajor = 0; // scale index of "Major"

        beginTest("a sung melody analyses to its notes and a pitch curve");
        {
            std::vector<float> audio((size_t) (2.4 * kRate), 0.0f);
            const int midis[] = { 57, 60, 64 }; // A3, C4, E4
            for (int i = 0; i < 3; ++i)
                addBurst(audio, midiToHz(midis[i]), 0.2 + 0.7 * i, 0.5);
            const auto t = take::analyse(audio.data(), (int) audio.size(), kRate, {}, 0.5f);
            expectEquals((int) t.notes.size(), 3);
            expect(! t.curve.empty(), "curve recorded");
            expect(! t.isEmpty(), "take is not empty");
            for (size_t i = 0; i < t.notes.size() && i < 3; ++i)
            {
                expectEquals(t.notes[i].detectedNote, midis[i]);
                expectEquals(t.notes[i].targetNote, midis[i]);
                expectWithinAbsoluteError(t.notes[i].sungMidi, (float) midis[i], 0.3f);
                expect(t.notes[i].end > t.notes[i].start, "positive length");
                expect(t.notes[i].velocity > 0.0f, "has velocity");
                if (i > 0)
                    expect(t.notes[i].start > t.notes[i - 1].start, "ordered starts");
            }
        }

        beginTest("silence analyses to an empty take");
        {
            std::vector<float> audio((size_t) kRate, 0.0f);
            const auto t = take::analyse(audio.data(), (int) audio.size(), kRate, {}, 0.5f);
            expect(t.notes.empty(), "no notes");
        }

        beginTest("shiftNote moves by semitones and clamps");
        {
            auto t = takeWith({ 60, 127, 0 });
            expect(take::shiftNote(t, 0, 3, false, 0, cMajor), "valid index");
            expectEquals(t.notes[0].targetNote, 63);
            expectEquals(t.notes[0].detectedNote, 60);
            take::shiftNote(t, 1, 5, false, 0, cMajor);
            expectEquals(t.notes[1].targetNote, 127);
            take::shiftNote(t, 2, -5, false, 0, cMajor);
            expectEquals(t.notes[2].targetNote, 0);
        }

        beginTest("shiftNote scale-locked steps by degrees");
        {
            auto t = takeWith({ 64, 64, 64, 61 }); // E4 in C major, and C#4 (off scale)
            take::shiftNote(t, 0, 1, true, 0, cMajor);
            expectEquals(t.notes[0].targetNote, 65); // F
            take::shiftNote(t, 1, 2, true, 0, cMajor);
            expectEquals(t.notes[1].targetNote, 67); // G
            take::shiftNote(t, 2, -2, true, 0, cMajor);
            expectEquals(t.notes[2].targetNote, 60); // C
            take::shiftNote(t, 3, 1, true, 0, cMajor);
            expectEquals(t.notes[3].targetNote, 62); // D, next legal above C#
        }

        beginTest("shiftNote with a bad index is a no-op returning false");
        {
            auto t = takeWith({ 60 });
            expect(! take::shiftNote(t, -1, 1, false, 0, cMajor), "negative");
            expect(! take::shiftNote(t, 1, 1, false, 0, cMajor), "past the end");
            expectEquals(t.notes[0].targetNote, 60);
        }

        beginTest("snapAllToScale snaps to the nearest scale note, ties down");
        {
            auto t = takeWith({ 61, 63, 66, 60 }); // C#, D#, F#, C in C major
            take::snapAllToScale(t, 0, cMajor);
            expectEquals(t.notes[0].targetNote, 60); // tie between C and D goes down
            expectEquals(t.notes[1].targetNote, 62); // tie between D and E goes down
            expectEquals(t.notes[2].targetNote, 65); // tie between F and G goes down
            expectEquals(t.notes[3].targetNote, 60);
        }

        beginTest("resetNote restores the detected note and drift");
        {
            auto t = takeWith({ 60 });
            take::shiftNote(t, 0, 4, false, 0, cMajor);
            t.notes[0].drift = 0.2f;
            expect(take::resetNote(t, 0), "valid index");
            expectEquals(t.notes[0].targetNote, 60);
            expectEquals(t.notes[0].drift, 1.0f);
            expect(! take::resetNote(t, 5), "bad index");
        }

        beginTest("deleteNote removes only that note");
        {
            auto t = takeWith({ 60, 62, 64 });
            expect(take::deleteNote(t, 1), "valid index");
            expectEquals((int) t.notes.size(), 2);
            expectEquals(t.notes[1].targetNote, 64);
            expect(! take::deleteNote(t, 2), "bad index");
        }

        beginTest("noteAt hits by time and target lane");
        {
            auto t = takeWith({ 60, 62 }); // 0..0.5 and 0.6..1.1
            take::shiftNote(t, 0, 2, false, 0, cMajor); // note 0 now draws in lane 62
            expectEquals(take::noteAt(t, 0.25, 62), 0);
            expectEquals(take::noteAt(t, 0.25, 60), -1); // its old lane is empty
            expectEquals(take::noteAt(t, 0.8, 62), 1);
            expectEquals(take::noteAt(t, 0.55, 62), -1); // the gap
            expectEquals(take::noteAt(t, 0.5, 62), -1);  // end is exclusive
        }

        beginTest("history undoes edits in order and reports empty");
        {
            auto t = takeWith({ 60 });
            take::TakeHistory h;
            expect(! h.canUndo(), "starts empty");
            expect(! h.undo(t), "nothing to undo");
            h.push(t);
            take::shiftNote(t, 0, 1, false, 0, cMajor);
            h.push(t);
            take::deleteNote(t, 0);
            expect(h.canUndo(), "can undo");
            expect(h.undo(t), "undo delete");
            expectEquals((int) t.notes.size(), 1);
            expectEquals(t.notes[0].targetNote, 61);
            expect(h.undo(t), "undo shift");
            expectEquals(t.notes[0].targetNote, 60);
            expect(! h.canUndo(), "drained");
            h.push(t);
            h.clear();
            expect(! h.canUndo(), "clear empties");
        }

        beginTest("history is capped at 100 entries");
        {
            auto t = takeWith({ 60 });
            take::TakeHistory h;
            for (int i = 0; i < 150; ++i)
            {
                h.push(t);
                take::shiftNote(t, 0, 1, false, 0, cMajor);
            }
            int undone = 0;
            while (h.undo(t))
                ++undone;
            expectEquals(undone, 100);
            expectEquals(t.notes[0].targetNote, 60 + 50); // oldest 50 snapshots were dropped
        }

        beginTest("ValueTree round trip keeps notes, anchor, tempo and path");
        {
            auto t = takeWith({ 60, 67 });
            t.sampleRate = 48000.0;
            t.anchorPpq = 16.5;
            t.anchorBpm = 98.0;
            t.audioPath = "C:/takes/a.wav";
            t.notes[1].targetNote = 69;
            t.notes[1].drift = 0.25f;
            t.notes[1].velocity = 0.75f;
            t.notes[1].sungMidi = 66.8f;
            t.curve.push_back({ 0.1, 60.0f, 0.9f, 0.1f });
            const auto r = take::fromValueTree(take::toValueTree(t));
            expectEquals(r.sampleRate, 48000.0);
            expect(r.anchorPpq.has_value(), "anchor present");
            expectEquals(r.anchorPpq.value_or(0.0), 16.5);
            expectEquals(r.anchorBpm, 98.0);
            expectEquals(r.audioPath, t.audioPath);
            expect(r.curve.empty(), "curve is not saved");
            expectEquals((int) r.notes.size(), 2);
            expectEquals(r.notes[1].detectedNote, 67);
            expectEquals(r.notes[1].targetNote, 69);
            expectEquals(r.notes[1].drift, 0.25f);
            expectEquals(r.notes[1].velocity, 0.75f);
            expectEquals(r.notes[1].sungMidi, 66.8f);
            expectEquals(r.notes[1].start, t.notes[1].start);
            expectEquals(r.notes[1].end, t.notes[1].end);
        }

        beginTest("ValueTree round trip with no anchor leaves it absent");
        {
            auto t = takeWith({ 60 });
            const auto tree = take::toValueTree(t);
            expect(! tree.hasProperty("anchor"), "no anchor property written");
            expect(! take::fromValueTree(tree).anchorPpq.has_value(), "still absent");
        }

        beginTest("fromValueTree on garbage gives an empty take");
        {
            expect(take::fromValueTree(juce::ValueTree()).isEmpty(), "invalid tree");
            expect(take::fromValueTree(juce::ValueTree("SOMETHING_ELSE")).isEmpty(), "wrong type");
        }
    }
};

static TakeTests takeTests;
} // namespace murmur::tests
