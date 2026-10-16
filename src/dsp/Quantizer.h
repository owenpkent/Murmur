#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

// Quantizer: "dynamic" quantize for sung and beatboxed MIDI, in two independent parts.
//
// GridQuantizer. Why this algorithm: hard quantize makes a human take sound robotic and
// fails badly when a note is closer to the "wrong" grid line. A strength control (move the
// note only part of the way to the grid) keeps the groove, a snap window leaves deliberate
// off-grid notes alone, and swing delays every second grid point so the result can feel
// shuffled. new = old + strength * (nearest grid point - old), applied only when the note
// is within the window. All functions are pure and static.
//
// GridInference. Why this algorithm: with the DAW stopped there is no host tempo, but the
// performer still has one in their head. Following Dixon's IOI-clustering approach (as in
// BeatRoot), we histogram the inter-onset intervals between recent onsets (not just
// neighbours, so skipped beats still vote), merge them into 25 ms clusters, and score each
// cluster by how many other clusters sit at integer multiples of it. The best-supported
// interval is the beat period; half/double ambiguity is resolved by folding into 70-170
// BPM. Phase comes from a circular mean of the onsets against that period, and both tempo
// and phase are nudged with exponential smoothing so the grid follows the player rather
// than jumping. WHEN THE DAW TRANSPORT IS PLAYING THE HOST TEMPO AND PPQ OVERRIDE THIS
// COMPLETELY: inference is only the free-tempo fallback.
//
// Upgrade path (rejected for v1): full BeatRoot multi-agent tracking, or a neural tempo
// model (madmom-style), would track tempo drift better; both fit behind update()/get().
namespace murmur::dsp
{
class GridQuantizer
{
public:
    enum class Grid { Quarter = 0, Eighth, Sixteenth, EighthTriplet, SixteenthTriplet, ThirtySecond };

    static double gridBeats(Grid g)
    {
        switch (g)
        {
            case Grid::Quarter: return 1.0;
            case Grid::Eighth: return 0.5;
            case Grid::Sixteenth: return 0.25;
            case Grid::EighthTriplet: return 1.0 / 3.0;
            case Grid::SixteenthTriplet: return 1.0 / 6.0;
            case Grid::ThirtySecond: return 0.125;
        }
        return 0.25;
    }

    // Position of grid point `index`. Odd points are delayed by swing (0..1) up to a full
    // triplet feel (one third of the grid step).
    static double gridPoint(long index, double step, double swing)
    {
        const double off = (index % 2 != 0) ? juce::jlimit(0.0, 1.0, swing) * step / 3.0 : 0.0;
        return (double) index * step + off;
    }

    static double nearestGridPoint(double ppq, Grid g, double swing)
    {
        const double step = gridBeats(g);
        const long base = (long) std::floor(ppq / step);
        double best = gridPoint(base - 1, step, swing);
        for (long i = base; i <= base + 2; ++i)
        {
            const double p = gridPoint(i, step, swing);
            if (std::abs(p - ppq) < std::abs(best - ppq))
                best = p;
        }
        return best;
    }

    static double quantize(double ppq, Grid g, float strength, float swing, double snapWindowBeats)
    {
        const double target = nearestGridPoint(ppq, g, swing);
        if (std::abs(target - ppq) > snapWindowBeats)
            return ppq;
        return ppq + (double) juce::jlimit(0.0f, 1.0f, strength) * (target - ppq);
    }
};

class GridInference
{
public:
    struct Result
    {
        double bpm = 120.0;
        double phaseSeconds = 0.0; // time of a beat, in [0, beat period)
        float confidence = 0.0f;   // 0..1; below ~0.5 the grid should not be trusted
    };

    void prepare(double, int) { reset(); }

    void reset()
    {
        count = 0;
        have = false;
        res = {};
    }

    // Report an onset time (seconds, monotonic). Re-estimates the grid.
    void addOnset(double t)
    {
        if (count == maxOnsets)
        {
            std::copy(onsets.begin() + 1, onsets.end(), onsets.begin());
            --count;
        }
        onsets[(size_t) count++] = t;
        update();
    }

    const Result& get() const { return res; }
    bool hasEstimate() const { return have; }

    // Convert a time in seconds to beats on the inferred grid (beat 0 at phaseSeconds).
    double secondsToBeats(double t) const { return (t - res.phaseSeconds) * res.bpm / 60.0; }
    double beatsToSeconds(double b) const { return res.phaseSeconds + b * 60.0 / res.bpm; }

    float smoothing = 0.3f; // exponential smoothing factor per update

private:
    static constexpr int maxOnsets = 32;
    static constexpr int maxLookAhead = 4; // pair each onset with the next few
    static constexpr double clusterWidth = 0.025;
    static constexpr double minIoi = 0.10, maxIoi = 2.5;

    struct Cluster { double interval = 0.0; int size = 0; double sum = 0.0; };

    void update()
    {
        if (count < 3)
            return;
        // Collect IOIs.
        std::array<double, maxOnsets * maxLookAhead> iois {};
        int ni = 0;
        for (int i = 0; i < count; ++i)
            for (int j = i + 1; j <= std::min(count - 1, i + maxLookAhead); ++j)
            {
                const double d = onsets[(size_t) j] - onsets[(size_t) i];
                if (d >= minIoi && d <= maxIoi)
                    iois[(size_t) ni++] = d;
            }
        if (ni == 0)
            return;
        std::sort(iois.begin(), iois.begin() + ni);

        // Greedy clustering on the sorted list.
        std::array<Cluster, maxOnsets * maxLookAhead> cl {};
        int nc = 0;
        for (int i = 0; i < ni; ++i)
        {
            if (nc > 0 && iois[(size_t) i] - cl[(size_t) (nc - 1)].interval < clusterWidth)
            {
                auto& c = cl[(size_t) (nc - 1)];
                c.sum += iois[(size_t) i];
                ++c.size;
                c.interval = c.sum / c.size;
            }
            else
                cl[(size_t) nc++] = { iois[(size_t) i], 1, iois[(size_t) i] };
        }

        // Score: each cluster earns support from clusters at integer multiples of it.
        int bestIdx = 0;
        double bestScore = -1.0, totalScore = 0.0;
        for (int i = 0; i < nc; ++i)
        {
            double score = 0.0;
            for (int j = 0; j < nc; ++j)
            {
                const double ratio = cl[(size_t) j].interval / cl[(size_t) i].interval;
                const int n = (int) std::lround(ratio);
                if (n >= 1 && n <= 4 && std::abs(cl[(size_t) j].interval - n * cl[(size_t) i].interval) < clusterWidth * n)
                    score += (double) cl[(size_t) j].size * (double) (6 - n);
            }
            totalScore += score;
            if (score > bestScore)
            {
                bestScore = score;
                bestIdx = i;
            }
        }
        double period = cl[(size_t) bestIdx].interval;
        double bpm = 60.0 / period;
        while (bpm < 70.0) bpm *= 2.0;
        while (bpm > 170.0) bpm *= 0.5;
        period = 60.0 / bpm;

        // Phase: circular mean of onset positions within the period.
        std::complex<double> acc { 0.0, 0.0 };
        for (int i = 0; i < count; ++i)
            acc += std::polar(1.0, 2.0 * juce::MathConstants<double>::pi * onsets[(size_t) i] / period);
        const double resultant = std::abs(acc) / count;
        double phase = std::arg(acc) / (2.0 * juce::MathConstants<double>::pi) * period;
        phase -= std::floor(phase / period) * period;

        const float conf = (float) juce::jlimit(0.0, 1.0, resultant * std::min(1.0, count / 6.0)
                                                              * (totalScore > 0.0 ? 0.5 + 0.5 * bestScore / totalScore : 0.0));

        if (! have)
        {
            res = { bpm, phase, conf };
            have = true;
            return;
        }
        const double rel = std::abs(bpm - res.bpm) / res.bpm;
        if (rel > 0.15 && conf < 0.7f)
        {
            res.confidence = std::min(res.confidence, conf); // contradicts the lock: ignore the tempo
            return;
        }
        const double a = rel > 0.15 ? 1.0 : (double) smoothing;
        res.bpm += a * (bpm - res.bpm);
        const double p = 60.0 / res.bpm;
        double dphase = phase - std::fmod(res.phaseSeconds, p);
        dphase -= std::round(dphase / p) * p; // shortest way round the circle
        res.phaseSeconds += a * dphase;
        res.phaseSeconds -= std::floor(res.phaseSeconds / p) * p;
        res.confidence += (float) a * (conf - res.confidence);
    }

    std::array<double, maxOnsets> onsets {};
    int count = 0;
    bool have = false;
    Result res;
};
} // namespace murmur::dsp
