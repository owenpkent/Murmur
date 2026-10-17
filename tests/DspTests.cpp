#include "dsp/DrumClassifier.h"
#include "dsp/DrumFeatures.h"
#include "dsp/MurmurEngine.h"
#include "dsp/NoteSegmenter.h"
#include "dsp/OnsetDetector.h"
#include "dsp/PitchTracker.h"
#include "dsp/Quantizer.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

// Pure-DSP tests for the Murmur engine. No plugin, editor or audio device: synthetic
// signals in, assertions out (mirrors the kit's UnitTest + ctest convention).
using namespace murmur::dsp;

namespace
{
constexpr double kSr = 44100.0;
constexpr double kTwoPi = 6.283185307179586;

std::vector<float> sine(double hz, double seconds, float amp = 0.5f)
{
    std::vector<float> v((size_t) (seconds * kSr));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = amp * (float) std::sin(kTwoPi * hz * (double) i / kSr);
    return v;
}

// Tone burst with raised-cosine attack/release, placed at `startSec` in a buffer of `totalSec`.
std::vector<float> toneBurst(double hz, double startSec, double lenSec, double totalSec)
{
    std::vector<float> v((size_t) (totalSec * kSr), 0.0f);
    const int a = (int) (startSec * kSr), n = (int) (lenSec * kSr), ramp = (int) (0.02 * kSr), atk = (int) (0.008 * kSr);
    for (int i = 0; i < n && a + i < (int) v.size(); ++i)
    {
        float env = 1.0f;
        if (i < atk) env = 0.5f - 0.5f * (float) std::cos(3.14159265 * i / atk);
        if (i > n - ramp) env = 0.5f - 0.5f * (float) std::cos(3.14159265 * (n - i) / ramp);
        const double ph = kTwoPi * hz * (double) i / kSr;
        v[(size_t) (a + i)] = 0.4f * env * (float) (std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.25 * std::sin(3 * ph));
    }
    return v;
}

template <typename Fn>
void feedInBlocks(const std::vector<float>& x, int block, Fn&& fn)
{
    for (size_t i = 0; i < x.size(); i += (size_t) block)
        fn(x.data() + i, (int) std::min<size_t>((size_t) block, x.size() - i));
}
} // namespace

struct PitchTrackerTests : juce::UnitTest
{
    PitchTrackerTests() : juce::UnitTest("PitchTracker") {}

    void runTest() override
    {
        beginTest("hzToMidi");
        expectWithinAbsoluteError(PitchTracker::hzToMidi(440.0), 69.0, 1.0e-9);
        expectWithinAbsoluteError(PitchTracker::hzToMidi(220.0), 57.0, 1.0e-9);

        beginTest("finds 220 Hz sine within 1 Hz with high clarity");
        {
            PitchTracker t;
            t.prepare(kSr, 512);
            feedInBlocks(sine(220.0, 0.5), 512, [&](const float* p, int n) { t.process(p, n); });
            PitchFrame f, last;
            int frames = 0;
            while (t.pop(f)) { last = f; ++frames; }
            expect(frames > 10);
            expectWithinAbsoluteError(last.f0Hz, 220.0, 1.0);
            expect(last.clarity > 0.9f);
        }

        beginTest("finds low and high voice-range pitches");
        for (double hz : { 82.4, 440.0, 880.0 })
        {
            PitchTracker t;
            t.prepare(kSr, 512);
            feedInBlocks(sine(hz, 0.5), 512, [&](const float* p, int n) { t.process(p, n); });
            PitchFrame f, last;
            while (t.pop(f)) last = f;
            expectWithinAbsoluteError(last.f0Hz, hz, hz * 0.01);
        }

        beginTest("silence reports low clarity");
        {
            PitchTracker t;
            t.prepare(kSr, 512);
            std::vector<float> z((size_t) (0.3 * kSr), 0.0f);
            feedInBlocks(z, 512, [&](const float* p, int n) { t.process(p, n); });
            PitchFrame f;
            int frames = 0;
            while (t.pop(f)) { ++frames; expect(f.clarity < 0.3f); }
            expect(frames > 0);
        }
    }
};
static PitchTrackerTests pitchTrackerTests;

struct OnsetDetectorTests : juce::UnitTest
{
    OnsetDetectorTests() : juce::UnitTest("OnsetDetector") {}

    static int countOnsets(const std::vector<float>& x, juce::int64 afterSample = 0)
    {
        OnsetDetector d;
        d.prepare(kSr, 512);
        d.setSensitivity(0.5f);
        int n = 0;
        feedInBlocks(x, 512, [&](const float* p, int len) {
            d.process(p, len);
            OnsetEvent e;
            while (d.pop(e)) if (e.samplePos >= afterSample) ++n;
        });
        return n;
    }

    void runTest() override
    {
        beginTest("fires once for a single burst");
        expectEquals(countOnsets(toneBurst(330.0, 0.3, 0.25, 1.0)), 1);

        beginTest("does not fire for a steady tone");
        expectEquals(countOnsets(sine(440.0, 1.0), (juce::int64) (0.2 * kSr)), 0);

        beginTest("fires for each of two separated bursts");
        {
            auto a = toneBurst(330.0, 0.2, 0.15, 1.0);
            auto b = toneBurst(330.0, 0.6, 0.15, 1.0);
            for (size_t i = 0; i < a.size(); ++i) a[i] += b[i];
            expectEquals(countOnsets(a), 2);
        }
    }
};
static OnsetDetectorTests onsetDetectorTests;

struct QuantizerTests : juce::UnitTest
{
    QuantizerTests() : juce::UnitTest("Quantizer") {}

    void runTest() override
    {
        using G = GridQuantizer::Grid;
        beginTest("grid sizes");
        expectWithinAbsoluteError(GridQuantizer::gridBeats(G::Quarter), 1.0, 1e-12);
        expectWithinAbsoluteError(GridQuantizer::gridBeats(G::EighthTriplet), 1.0 / 3.0, 1e-12);
        expectWithinAbsoluteError(GridQuantizer::gridBeats(G::ThirtySecond), 0.125, 1e-12);

        beginTest("strength 0 is identity");
        expectWithinAbsoluteError(GridQuantizer::quantize(1.37, G::Sixteenth, 0.0f, 0.0f, 10.0), 1.37, 1e-12);

        beginTest("strength 1 lands on the grid");
        expectWithinAbsoluteError(GridQuantizer::quantize(1.37, G::Sixteenth, 1.0f, 0.0f, 10.0), 1.25, 1e-9);
        expectWithinAbsoluteError(GridQuantizer::quantize(1.40, G::Sixteenth, 1.0f, 0.0f, 10.0), 1.50, 1e-9);

        beginTest("partial strength moves part of the way");
        expectWithinAbsoluteError(GridQuantizer::quantize(1.37, G::Sixteenth, 0.5f, 0.0f, 10.0), 1.31, 1e-9);

        beginTest("swing moves only odd grid points");
        expectWithinAbsoluteError(GridQuantizer::quantize(1.0, G::Eighth, 1.0f, 1.0f, 10.0), 1.0, 1e-9); // even point
        expectWithinAbsoluteError(GridQuantizer::quantize(0.0, G::Eighth, 1.0f, 1.0f, 10.0), 0.0, 1e-9); // even point
        expectWithinAbsoluteError(GridQuantizer::quantize(0.5, G::Eighth, 1.0f, 1.0f, 10.0), 0.5 + 0.5 / 3.0, 1e-9); // odd point
        expectWithinAbsoluteError(GridQuantizer::quantize(0.5, G::Eighth, 1.0f, 0.0f, 10.0), 0.5, 1e-9); // no swing

        beginTest("snap window excludes far notes");
        expectWithinAbsoluteError(GridQuantizer::quantize(1.37, G::Sixteenth, 1.0f, 0.0f, 0.05), 1.37, 1e-12);
        expectWithinAbsoluteError(GridQuantizer::quantize(1.26, G::Sixteenth, 1.0f, 0.0f, 0.05), 1.25, 1e-9);

        beginTest("grid inference recovers 120 BPM from a 0.5 s click train");
        {
            GridInference g;
            g.prepare(kSr, 512);
            for (int i = 0; i < 12; ++i)
                g.addOnset(0.1 + 0.5 * i);
            expect(g.hasEstimate());
            expectWithinAbsoluteError(g.get().bpm, 120.0, 2.0);
            expect(g.get().confidence > 0.5f);
            const double p = 0.5;
            double ph = std::fmod(g.get().phaseSeconds - 0.1, p);
            if (ph < 0) ph += p;
            expect(std::min(ph, p - ph) < 0.02);
        }

        beginTest("grid inference folds double-time into 70-170 BPM");
        {
            GridInference g;
            for (int i = 0; i < 20; ++i)
                g.addOnset(0.25 * i); // 240 BPM clicks
            expect(g.get().bpm >= 70.0 && g.get().bpm <= 170.0);
        }
    }
};
static QuantizerTests quantizerTests;

struct NoteSegmenterTests : juce::UnitTest
{
    NoteSegmenterTests() : juce::UnitTest("NoteSegmenter") {}

    static constexpr int hop = 256;

    // Feeds frames covering [startFrame, startFrame+count) at the given pitch (0 = unvoiced).
    static void feed(NoteSegmenter& s, int startFrame, int count, double hz)
    {
        for (int i = 0; i < count; ++i)
        {
            PitchFrame f;
            f.samplePos = 5000 + (juce::int64) (startFrame + i) * hop;
            f.f0Hz = hz;
            f.clarity = hz > 0.0 ? 0.9f : 0.0f;
            f.rms = hz > 0.0 ? 0.1f : 0.0f;
            s.pushFrame(f);
        }
    }

    static std::vector<NoteEvent> drain(NoteSegmenter& s)
    {
        std::vector<NoteEvent> v;
        NoteEvent e;
        while (s.pop(e)) v.push_back(e);
        return v;
    }

    void runTest() override
    {
        beginTest("300 ms 440 Hz tone gives On then Off");
        {
            NoteSegmenter s;
            s.prepare(kSr, 512);
            feed(s, 0, 52, 440.0);  // ~300 ms
            feed(s, 52, 20, 0.0);   // silence
            const auto ev = drain(s);
            expectEquals((int) ev.size(), 2);
            if (ev.size() == 2)
            {
                expect(ev[0].type == NoteEvent::On);
                expect(ev[1].type == NoteEvent::Off);
                expectEquals(ev[0].midiNote, 69);
                expectEquals(ev[1].midiNote, 69);
                expect(ev[0].velocity > 0.0f && ev[0].velocity <= 1.0f);
                expect(ev[1].samplePos - ev[0].samplePos >= (juce::int64) (0.06 * kSr));
                expectWithinAbsoluteError(ev[0].cents, 0.0, 1.0);
            }
        }

        beginTest("pitch scoop does not spray notes, and short blips are ignored");
        {
            NoteSegmenter s;
            s.prepare(kSr, 512);
            feed(s, 0, 3, 440.0); // 17 ms blip
            feed(s, 3, 20, 0.0);
            expectEquals((int) drain(s).size(), 0);
        }

        beginTest("onset on the same pitch retriggers: two notes");
        {
            NoteSegmenter s;
            s.prepare(kSr, 512);
            feed(s, 0, 30, 440.0);
            OnsetEvent o;
            o.samplePos = 5000 + 30 * hop;
            s.pushOnset(o);
            feed(s, 30, 30, 440.0);
            feed(s, 60, 20, 0.0);
            const auto ev = drain(s);
            int ons = 0, offs = 0;
            for (auto& e : ev) { if (e.type == NoteEvent::On) ++ons; else ++offs; }
            expectEquals(ons, 2);
            expectEquals(offs, 2);
        }

        beginTest("pitch change beyond 60 cents that holds retriggers");
        {
            NoteSegmenter s;
            s.prepare(kSr, 512);
            feed(s, 0, 30, 440.0);
            feed(s, 30, 30, 493.88); // B4, a whole tone up
            feed(s, 60, 20, 0.0);
            const auto ev = drain(s);
            int ons = 0;
            for (auto& e : ev) if (e.type == NoteEvent::On) ++ons;
            expectEquals(ons, 2);
            if (ev.size() >= 3) expectEquals(ev[2].midiNote, 71);
        }

        beginTest("vibrato within 60 cents does not retrigger");
        {
            NoteSegmenter s;
            s.prepare(kSr, 512);
            feed(s, 0, 15, 440.0);
            feed(s, 15, 15, 440.0 * std::pow(2.0, 40.0 / 1200.0));
            feed(s, 30, 15, 440.0);
            feed(s, 45, 20, 0.0);
            int ons = 0;
            for (auto& e : drain(s)) if (e.type == NoteEvent::On) ++ons;
            expectEquals(ons, 1);
        }

        beginTest("scale lock snaps out-of-scale pitches");
        {
            NoteSegmenter s;
            s.prepare(kSr, 512);
            s.params.scaleLock = true;
            s.params.rootPitchClass = 0;
            s.params.scaleIndex = 0; // first entry of the kit's scale table
            feed(s, 0, 30, 440.0 * std::pow(2.0, 1.0 / 12.0)); // A#4 = 70
            const auto ev = drain(s);
            expectEquals((int) ev.size(), 1);
            if (! ev.empty())
                expect(okstudio::scales::isInScale(ev[0].midiNote, 0, 0));
        }
    }
};
static NoteSegmenterTests noteSegmenterTests;

struct DrumTests : juce::UnitTest
{
    DrumTests() : juce::UnitTest("Drums") {}

    // Builds a raw (un-normalised) feature vector for tests.
    static FeatureVector raw(float hz, float flat, float zcr, float lo, float mid, float hi, float db)
    {
        return drumnorm::normalise({ std::log(hz), flat, zcr, lo, mid, hi, db });
    }

    void runTest() override
    {
        beginTest("default MIDI mapping");
        expectEquals(DrumClassifier::defaultMidiNote(DrumClassifier::Class::Kick), 36);
        expectEquals(DrumClassifier::defaultMidiNote(DrumClassifier::Class::Snare), 38);
        expectEquals(DrumClassifier::defaultMidiNote(DrumClassifier::Class::HatClosed), 42);
        expectEquals(DrumClassifier::defaultMidiNote(DrumClassifier::Class::HatOpen), 46);

        using C = DrumClassifier::Class;
        beginTest("defaults classify before calibration");
        {
            DrumClassifier c;
            expect(c.classify(raw(130, 0.03f, 0.03f, 0.9f, 0.09f, 0.01f, -20)).cls == C::Kick);
            expect(c.classify(raw(2500, 0.25f, 0.2f, 0.1f, 0.55f, 0.35f, -24)).cls == C::Snare);
            expect(c.classify(raw(7500, 0.45f, 0.42f, 0.0f, 0.08f, 0.92f, -33)).cls == C::HatClosed);
        }

        beginTest("returns Kick for a low-centroid vector after calibration");
        {
            DrumClassifier c;
            c.addExample(C::Kick, raw(90, 0.02f, 0.02f, 0.95f, 0.05f, 0.0f, -15));
            c.addExample(C::Kick, raw(110, 0.03f, 0.03f, 0.93f, 0.06f, 0.01f, -17));
            c.addExample(C::Kick, raw(100, 0.02f, 0.02f, 0.94f, 0.06f, 0.0f, -16));
            c.addExample(C::Snare, raw(2000, 0.3f, 0.2f, 0.1f, 0.6f, 0.3f, -20));
            c.addExample(C::Snare, raw(2400, 0.3f, 0.22f, 0.1f, 0.55f, 0.35f, -22));
            c.addExample(C::HatClosed, raw(8000, 0.5f, 0.45f, 0.0f, 0.05f, 0.95f, -30));
            const auto r = c.classify(raw(105, 0.025f, 0.025f, 0.94f, 0.055f, 0.005f, -16));
            expect(r.cls == C::Kick);
            expect(r.confidence > 0.5f);
            expectEquals(c.getNumExamples(C::Kick), 3);
            c.clearClass(C::Kick);
            expectEquals(c.getNumExamples(C::Kick), 0);
        }

        beginTest("calibration round-trips through ValueTree");
        {
            DrumClassifier a;
            a.addExample(C::Snare, raw(2100, 0.3f, 0.2f, 0.1f, 0.6f, 0.3f, -20));
            a.addExample(C::HatOpen, raw(6000, 0.5f, 0.4f, 0.0f, 0.1f, 0.9f, -22));
            DrumClassifier b;
            b.fromValueTree(a.toValueTree());
            expectEquals(b.getNumExamples(C::Snare), 1);
            expectEquals(b.getNumExamples(C::HatOpen), 1);
            expectEquals(b.getNumExamples(C::Kick), 0);
        }

        beginTest("feature extractor separates a thump from a hiss");
        {
            DrumFeatureExtractor ex;
            ex.prepare(kSr, 512);
            const int n = ex.getWindowLength();
            std::vector<float> thump((size_t) n), hiss((size_t) n);
            juce::Random rng(7);
            for (int i = 0; i < n; ++i)
            {
                const float env = std::exp(-(float) i / (0.006f * (float) kSr));
                thump[(size_t) i] = 0.8f * env * (float) std::sin(kTwoPi * 70.0 * i / kSr);
                hiss[(size_t) i] = 0.3f * env * (rng.nextFloat() * 2.0f - 1.0f);
            }
            const auto ft = ex.extract(thump.data());
            const auto fh = ex.extract(hiss.data());
            expect(ft.v[FeatureVector::CentroidLogHz] < fh.v[FeatureVector::CentroidLogHz]);
            expect(ft.v[FeatureVector::LowRatio] > fh.v[FeatureVector::LowRatio]);
            expect(ft.v[FeatureVector::Zcr] < fh.v[FeatureVector::Zcr]);
            DrumClassifier c;
            expect(c.classify(ft).cls == C::Kick);
        }
    }
};
static DrumTests drumTests;

struct MurmurEngineTests : juce::UnitTest
{
    MurmurEngineTests() : juce::UnitTest("MurmurEngine") {}

    void runTest() override
    {
        beginTest("latency is 20 ms");
        {
            MurmurEngine e;
            e.prepare(kSr, 512);
            expectEquals(e.getLatencySamples(), (int) std::lround(0.02 * kSr));
        }

        beginTest("melody mode turns a sung tone into note on and off");
        {
            MurmurEngine e;
            e.prepare(kSr, 512);
            e.setMode(MurmurEngine::Mode::Melody);
            std::vector<MurmurEngine::MidiOut> out;
            out.reserve(64);
            juce::int64 pos = 0;
            feedInBlocks(toneBurst(440.0, 0.1, 0.4, 1.0), 512, [&](const float* p, int n) {
                e.setTransport(0.0, 120.0, false);
                e.process(p, n, pos);
                pos += n;
                e.drainMidi(out);
            });
            int on = 0, off = 0, note = -1;
            for (auto& m : out)
            {
                if (m.msg.isNoteOn()) { ++on; note = m.msg.getNoteNumber(); }
                if (m.msg.isNoteOff()) ++off;
            }
            expectEquals(on, 1);
            expectEquals(off, 1);
            expectEquals(note, 69);
        }

        beginTest("drums mode emits a hit for a thump");
        {
            MurmurEngine e;
            e.prepare(kSr, 512);
            e.setMode(MurmurEngine::Mode::Drums);
            std::vector<float> x((size_t) kSr, 0.0f);
            const int a = (int) (0.2 * kSr);
            for (int i = 0; i < (int) (0.15 * kSr); ++i)
                x[(size_t) (a + i)] = 0.8f * std::exp(-(float) i / (0.03f * (float) kSr)) * (float) std::sin(kTwoPi * 60.0 * i / kSr);
            std::vector<MurmurEngine::MidiOut> out;
            out.reserve(64);
            juce::int64 pos = 0;
            feedInBlocks(x, 512, [&](const float* p, int n) {
                e.setTransport(0.0, 120.0, false);
                e.process(p, n, pos);
                pos += n;
                e.drainMidi(out);
            });
            int on = 0;
            for (auto& m : out)
                if (m.msg.isNoteOn()) ++on;
            expect(on >= 1);
        }
    }
};
static MurmurEngineTests murmurEngineTests;

int main()
{
    juce::UnitTestRunner runner;
    runner.runAllTests();
    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        if (const auto* r = runner.getResult(i))
            failures += r->failures;
    return failures == 0 ? 0 : 1;
}
