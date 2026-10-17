// Take Editor, Phase B (docs/TAKE_EDITOR_DESIGN.md): the offline pitch-shift render. Pure code
// on plain buffers, no processor needed. tests/DspTests.cpp owns the runner's main(); this file
// is self-registering statics only.

#include "../src/dsp/PitchTracker.h"
#include "../src/take/VoiceRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace murmur::tests
{
namespace
{
    constexpr double kRate = 48000.0;

    // A steady voiced tone (a few harmonics) with a take whose single note and flat pitch curve
    // describe it.
    struct Fixture
    {
        std::vector<float> audio;
        take::Take take;
    };

    Fixture makeTone(double hz, double startSec, double lenSec, double totalSec)
    {
        Fixture f;
        f.audio.assign((size_t) (totalSec * kRate), 0.0f);
        const int a = (int) (startSec * kRate), n = (int) (lenSec * kRate);
        for (int i = 0; i < n; ++i)
        {
            const double ph = 6.283185307179586 * hz * (double) i / kRate;
            f.audio[(size_t) (a + i)] = 0.4f * (float) (std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.25 * std::sin(3 * ph));
        }
        const float midi = (float) murmur::dsp::PitchTracker::hzToMidi(hz);
        f.take.sampleRate = kRate;
        for (double t = 0.0; t < totalSec; t += 256.0 / kRate)
        {
            take::PitchPoint p;
            p.seconds = t;
            p.midi = (t >= startSec && t < startSec + lenSec) ? midi : 0.0f;
            p.clarity = 0.9f;
            f.take.curve.push_back(p);
        }
        take::TakeNote tn;
        tn.start = startSec;
        tn.end = startSec + lenSec;
        tn.sungMidi = midi;
        tn.detectedNote = tn.targetNote = (int) std::lround(midi);
        f.take.notes.push_back(tn);
        return f;
    }

    // Median f0 (Hz) of clear voiced frames whose centre lies in [fromSec, toSec].
    double medianHz(const std::vector<float>& v, double fromSec, double toSec)
    {
        murmur::dsp::PitchTracker pt;
        pt.prepare(kRate, 512);
        std::vector<double> hz;
        murmur::dsp::PitchFrame fr;
        // Fed in blocks and drained after each: the tracker's frame queue is bounded, so one
        // big process() call would leave only the tail of a long buffer to measure.
        for (int pos = 0; pos < (int) v.size(); pos += 512)
        {
            pt.process(v.data() + pos, std::min(512, (int) v.size() - pos));
            while (pt.pop(fr))
            {
                const double t = (double) fr.samplePos / kRate;
                if (t >= fromSec && t <= toSec && fr.f0Hz > 0.0 && fr.clarity > 0.8f)
                    hz.push_back(fr.f0Hz);
            }
        }
        if (hz.empty())
            return 0.0;
        std::sort(hz.begin(), hz.end());
        return hz[hz.size() / 2];
    }

    float maxJump(const std::vector<float>& v, int from, int to)
    {
        float m = 0.0f;
        for (int i = std::max(1, from); i < to && i < (int) v.size(); ++i)
            m = std::max(m, std::abs(v[(size_t) i] - v[(size_t) (i - 1)]));
        return m;
    }

    // Measurements the run prints so they can be read, not asserted.
    void report(const juce::String& line) { std::puts(("    " + line).toRawUTF8()); }

    double rms(const std::vector<float>& v, int a, int b)
    {
        double e = 0.0;
        for (int i = a; i < b; ++i)
            e += (double) v[(size_t) i] * v[(size_t) i];
        return std::sqrt(e / std::max(1, b - a));
    }
} // namespace

class VoiceRendererTests : public juce::UnitTest
{
public:
    VoiceRendererTests() : juce::UnitTest("Voice renderer", "murmur") {}

    void runTest() override
    {
        using take::VoiceRenderer;

        beginTest("shiftAt: untouched, moved, drift 0 and unvoiced");
        {
            take::TakeNote n;
            n.detectedNote = n.targetNote = 60;
            expectWithinAbsoluteError(VoiceRenderer::shiftAt(n, 60.3f), 0.0f, 1.0e-5f);
            expect(! VoiceRenderer::needsRender(n), "untouched needs no render");

            n.targetNote = 62; // moved +2, drift 1: keeps the intonation offset
            expectWithinAbsoluteError(VoiceRenderer::shiftAt(n, 60.3f), 2.0f, 1.0e-5f);
            expectWithinAbsoluteError(VoiceRenderer::shiftAt(n, 59.8f), 2.0f, 1.0e-5f);
            expect(VoiceRenderer::needsRender(n), "moved needs a render");

            n.drift = 0.0f; // pinned to the centre
            expectWithinAbsoluteError(VoiceRenderer::shiftAt(n, 60.3f), 62.0f - 60.3f, 1.0e-5f);
            expectWithinAbsoluteError(VoiceRenderer::shiftAt(n, 59.8f), 62.0f - 59.8f, 1.0e-5f);

            n.drift = 0.5f; // halfway
            expectWithinAbsoluteError(VoiceRenderer::shiftAt(n, 60.4f), 62.0f + 0.5f * 0.4f - 60.4f, 1.0e-5f);

            expectWithinAbsoluteError(VoiceRenderer::shiftAt(n, 0.0f), 2.0f, 1.0e-5f); // unvoiced: plain move

            n.targetNote = n.detectedNote; // as sung but flattened still needs a render
            n.drift = 0.99f;
            expect(VoiceRenderer::needsRender(n), "drift below 1 needs a render");
        }

        beginTest("a sine shifted +12 renders an octave up inside the note and is untouched outside");
        {
            auto f = makeTone(220.0, 0.5, 1.0, 2.0);
            f.take.notes[0].targetNote += 12;
            auto corrected = f.audio;
            const auto t0 = std::chrono::steady_clock::now();
            VoiceRenderer::renderNote(f.audio, corrected, f.take, 0, kRate);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const double hz = medianHz(corrected, 0.75, 1.35);
            const double cents = 1200.0 * std::log2(hz / 440.0);
            report("one 1 s note: median " + juce::String(hz, 2) + " Hz (" + juce::String(cents, 1)
                       + " cents from 440), render " + juce::String(ms, 1) + " ms");
            expect(hz > 0.0 && std::abs(cents) < 15.0, "an octave up within 15 cents");

            const int pad = (int) std::lround(VoiceRenderer::padSeconds * kRate);
            const int a = (int) (0.5 * kRate) - pad, b = (int) (1.5 * kRate) + pad;
            bool same = true;
            for (int i = 0; i < (int) corrected.size(); ++i)
                if ((i < a || i >= b) && corrected[(size_t) i] != f.audio[(size_t) i])
                    same = false;
            expect(same, "samples outside the padded region are identical");
        }

        beginTest("a 0 shift forced through the stretcher is time-aligned and keeps its level");
        {
            auto f = makeTone(220.0, 0.5, 1.0, 2.0);
            f.take.notes[0].drift = 0.5f; // target == detected, frames on the centre: shift 0
            auto corrected = f.audio;
            VoiceRenderer::renderNoteForced(f.audio, corrected, f.take, 0, kRate);

            const int from = (int) (0.7 * kRate), to = (int) (1.3 * kRate), maxLag = (int) (0.005 * kRate);
            int bestLag = 0;
            double best = -1.0e30;
            for (int lag = -maxLag; lag <= maxLag; ++lag)
            {
                double c = 0.0;
                for (int i = from; i < to; ++i)
                    c += (double) f.audio[(size_t) i] * corrected[(size_t) (i + lag)];
                if (c > best)
                {
                    best = c;
                    bestLag = lag;
                }
            }
            report(("forced 0 shift: correlation peak at lag " + juce::String(bestLag) + " samples"));
            expect(std::abs(bestLag) <= (int) (0.001 * kRate), "peak within 1 ms of lag 0");
            const double ratio = rms(corrected, from, to) / rms(f.audio, from, to);
            expect(ratio > 0.85 && ratio < 1.15, "level within about 1 dB, got " + juce::String(ratio, 3));
        }

        beginTest("the crossfade edges have no discontinuity");
        {
            auto f = makeTone(220.0, 0.0, 2.0, 2.0); // sounding through the edges, so a seam would show
            f.take.notes[0].start = 0.5;
            f.take.notes[0].end = 1.5;
            f.take.notes[0].targetNote += 5;
            auto corrected = f.audio;
            VoiceRenderer::renderNote(f.audio, corrected, f.take, 0, kRate);
            const int pad = (int) std::lround(VoiceRenderer::padSeconds * kRate);
            const int a = (int) (0.5 * kRate) - pad, b = (int) (1.5 * kRate) + pad;
            const float sig = maxJump(f.audio, (int) (0.7 * kRate), (int) (1.3 * kRate));
            const float edgeA = maxJump(corrected, a - 50, a + 800), edgeB = maxJump(corrected, b - 800, b + 50);
            report("max jump: signal " + juce::String(sig, 3) + ", start edge " + juce::String(edgeA, 3)
                       + ", end edge " + juce::String(edgeB, 3));
            expect(edgeA < sig * 2.0f + 0.05f, "no click at the start edge");
            expect(edgeB < sig * 2.0f + 0.05f, "no click at the end edge");
        }

        beginTest("renderAll on an unedited take returns the original exactly");
        {
            auto f = makeTone(220.0, 0.5, 1.0, 2.0);
            expect(VoiceRenderer::renderAll(f.audio, f.take, kRate) == f.audio, "identical");
        }

        beginTest("reverting a note restores the original over its region");
        {
            auto f = makeTone(220.0, 0.5, 1.0, 2.0);
            f.take.notes[0].targetNote += 3;
            auto corrected = VoiceRenderer::renderAll(f.audio, f.take, kRate);
            expect(corrected != f.audio, "edited render differs");
            f.take.notes[0].targetNote = f.take.notes[0].detectedNote;
            VoiceRenderer::renderNote(f.audio, corrected, f.take, 0, kRate);
            expect(corrected == f.audio, "reverted note is back to the original");
        }

        beginTest("changedNotes and removedRegions");
        {
            auto f = makeTone(220.0, 0.5, 1.0, 2.0);
            take::TakeNote second = f.take.notes[0];
            second.start = 1.6;
            second.end = 1.9;
            f.take.notes.push_back(second);
            const auto snap = VoiceRenderer::snapshot(f.take);
            expect(VoiceRenderer::changedNotes(snap, f.take).empty(), "nothing changed");

            f.take.notes[1].targetNote += 2;
            auto ch = VoiceRenderer::changedNotes(snap, f.take);
            expectEquals((int) ch.size(), 1);
            if (! ch.empty())
                expectEquals(ch[0], 1);

            f.take.notes[0].drift = 0.2f;
            expectEquals((int) VoiceRenderer::changedNotes(snap, f.take).size(), 2);

            f.take.notes.erase(f.take.notes.begin());
            const auto gone = VoiceRenderer::removedRegions(snap, f.take);
            expectEquals((int) gone.size(), 1);
            if (! gone.empty())
                expectEquals(gone[0].first, 0.5);
            // The surviving note is now index 0 and is still found by its times.
            expectEquals((int) VoiceRenderer::changedNotes(snap, f.take).size(), 1);
        }

        beginTest("timing: a 10 s take at 48 kHz with every note edited");
        {
            const int count = 20;
            Fixture f;
            f.audio.assign((size_t) (10.0 * kRate), 0.0f);
            f.take.sampleRate = kRate;
            for (int i = 0; i < count; ++i)
            {
                auto one = makeTone(196.0 * std::pow(2.0, (i % 5) / 12.0), 0.0, 0.4, 0.5);
                const int off = (int) ((0.1 + 0.5 * i) * kRate);
                for (int s = 0; s < (int) (0.4 * kRate); ++s)
                    f.audio[(size_t) (off + s)] = one.audio[(size_t) s];
                take::TakeNote n = one.take.notes[0];
                n.start = 0.1 + 0.5 * i;
                n.end = n.start + 0.4;
                n.targetNote += 2;
                f.take.notes.push_back(n);
            }
            f.take.curve = makeTone(220.0, 0.0, 10.0, 10.0).take.curve;
            const auto t0 = std::chrono::steady_clock::now();
            const auto out = VoiceRenderer::renderAll(f.audio, f.take, kRate);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            report(("renderAll, 10 s take, " + juce::String(count) + " edited notes: " + juce::String(ms, 0) + " ms"));
            expectEquals((int) out.size(), (int) f.audio.size());
        }
    }
};

static VoiceRendererTests voiceRendererTests;
} // namespace murmur::tests
