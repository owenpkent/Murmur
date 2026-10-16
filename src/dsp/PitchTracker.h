#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

// PitchTracker: monophonic f0 tracking with the McLeod Pitch Method (MPM).
//
// Why this algorithm: a sung or hummed line is a single, strongly periodic voice, and the
// two things that ruin naive trackers on voice are octave errors (a strong 2nd harmonic
// makes plain autocorrelation pick half the period) and slow, noisy attacks. MPM divides
// the autocorrelation by the running energy of the two overlapping segments (the
// "normalised square difference function"), which bounds every peak to [-1, 1] and makes
// the height of the first strong peak a direct, level-independent periodicity score. We
// expose that score as `clarity`; the segmenter uses it as the voiced/unvoiced signal.
// Picking the FIRST peak within 93% of the tallest one is what kills octave errors.
// The NSDF is computed through one zero-padded FFT autocorrelation per hop, so the cost
// is flat and allocation-free after prepare().
//
// Upgrade path (rejected for v1 to keep the plugin dependency-free): Cycfi Q's bitstream
// autocorrelation pitch detector is lower latency and handles very low pitches better;
// aubio's YIN/YINFFT is a drop-in alternative. Either can sit behind this same API.
//
// Timing: a PitchFrame is emitted every hop once a full window has been seen.
// `samplePos` is the CENTRE of the analysed window (the instant the pitch describes), so
// a frame is always available roughly window/2 after the time it refers to.
namespace murmur::dsp
{
struct PitchFrame
{
    double f0Hz = 0.0;         // 0 when no periodicity was found
    float clarity = 0.0f;      // 0..1 NSDF peak height; ~1 for a clean tone
    float rms = 0.0f;          // RMS of the analysis window
    juce::int64 samplePos = 0; // centre of the analysis window, in input samples
};

class PitchTracker
{
public:
    static constexpr double minHz = 60.0;
    static constexpr double maxHz = 1200.0;

    static double hzToMidi(double hz) { return hz > 0.0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : 0.0; }

    void prepare(double sampleRate, int maxBlock)
    {
        juce::ignoreUnused(maxBlock);
        sr = sampleRate;
        const int scale = std::max(1, (int) std::lround(sr / 48000.0));
        window = 2048 * scale;
        hop = 256 * scale;
        minLag = std::max(2, (int) std::floor(sr / maxHz));
        maxLag = std::min(window - 2, (int) std::ceil(sr / minHz));
        int order = 1;
        while ((1 << order) < 2 * window)
            ++order;
        fftSize = 1 << order;
        fft = std::make_unique<juce::dsp::FFT>(order);
        ring.assign((size_t) window, 0.0f);
        lin.assign((size_t) window, 0.0f);
        fa.assign((size_t) fftSize, {});
        fb.assign((size_t) fftSize, {});
        nsdf.assign((size_t) window, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill(ring.begin(), ring.end(), 0.0f);
        writeIdx = 0;
        total = 0;
        sinceHop = 0;
        qHead = qCount = 0;
    }

    // Feed samples. Any number of frames (one per hop) may become available via pop().
    void process(const float* in, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) writeIdx] = in[i];
            writeIdx = (writeIdx + 1 == window) ? 0 : writeIdx + 1;
            ++total;
            if (++sinceHop >= hop)
            {
                sinceHop = 0;
                if (total >= window)
                    analyse();
            }
        }
    }

    bool pop(PitchFrame& out)
    {
        if (qCount == 0)
            return false;
        out = queue[(size_t) qHead];
        qHead = (qHead + 1) % queueSize;
        --qCount;
        return true;
    }

    int getWindowSize() const { return window; }
    int getHopSize() const { return hop; }

private:
    static constexpr int queueSize = 128;
    static constexpr float cutoffK = 0.93f; // MPM "k": first peak within k of the tallest

    void analyse()
    {
        // Linearise the ring oldest-first.
        double energy = 0.0;
        for (int i = 0; i < window; ++i)
        {
            const float v = ring[(size_t) ((writeIdx + i) % window)];
            lin[(size_t) i] = v;
            energy += (double) v * v;
        }
        PitchFrame f;
        f.samplePos = total - window / 2;
        f.rms = (float) std::sqrt(energy / window);

        if (f.rms > 1.0e-4f)
        {
            // Autocorrelation via FFT (zero padded to avoid circular wrap).
            for (int i = 0; i < fftSize; ++i)
                fa[(size_t) i] = { i < window ? lin[(size_t) i] : 0.0f, 0.0f };
            fft->perform(fa.data(), fb.data(), false);
            for (auto& c : fb)
                c = { std::norm(c), 0.0f };
            fft->perform(fb.data(), fa.data(), true);

            // The inverse transform's scaling differs by backend: normalise using the known r(0).
            const double r0 = fa[0].real();
            const double scale = r0 > 0.0 ? energy / r0 : 0.0;

            // NSDF: n(t) = 2 r(t) / m(t), with m(t) updated recursively.
            double m = 2.0 * energy;
            nsdf[0] = 1.0f;
            for (int t = 1; t <= maxLag + 1; ++t)
            {
                const double a = lin[(size_t) (t - 1)], b = lin[(size_t) (window - t)];
                m -= a * a + b * b;
                const double r = fa[(size_t) t].real() * scale;
                nsdf[(size_t) t] = m > 1.0e-12 ? (float) (2.0 * r / m) : 0.0f;
            }
            pickPeak(f);
        }
        if (qCount == queueSize) // drop oldest rather than allocate
        {
            qHead = (qHead + 1) % queueSize;
            --qCount;
        }
        queue[(size_t) ((qHead + qCount) % queueSize)] = f;
        ++qCount;
    }

    void pickPeak(PitchFrame& f)
    {
        std::array<int, 64> peaks {};
        int np = 0;
        const int last = maxLag;
        int t = 1;
        while (t <= last && nsdf[(size_t) t] > 0.0f)
            ++t; // skip the initial lobe at lag 0
        while (t <= last && np < (int) peaks.size())
        {
            while (t <= last && nsdf[(size_t) t] <= 0.0f)
                ++t;
            if (t > last)
                break;
            int best = t;
            while (t <= last && nsdf[(size_t) t] > 0.0f)
            {
                if (nsdf[(size_t) t] > nsdf[(size_t) best])
                    best = t;
                ++t;
            }
            if (best >= minLag && best > 1 && best < last)
                peaks[(size_t) np++] = best;
        }
        if (np == 0)
            return;
        float nmax = 0.0f;
        for (int i = 0; i < np; ++i)
            nmax = std::max(nmax, nsdf[(size_t) peaks[(size_t) i]]);
        for (int i = 0; i < np; ++i)
        {
            const int p = peaks[(size_t) i];
            if (nsdf[(size_t) p] >= cutoffK * nmax)
            {
                const float a = nsdf[(size_t) (p - 1)], b = nsdf[(size_t) p], c = nsdf[(size_t) (p + 1)];
                const float den = a - 2.0f * b + c;
                const float shift = std::abs(den) > 1.0e-9f ? 0.5f * (a - c) / den : 0.0f;
                const double tau = p + (double) shift;
                f.f0Hz = sr / tau;
                f.clarity = juce::jlimit(0.0f, 1.0f, b - 0.25f * (a - c) * shift);
                if (f.f0Hz < minHz * 0.9 || f.f0Hz > maxHz * 1.1)
                {
                    f.f0Hz = 0.0;
                    f.clarity = 0.0f;
                }
                return;
            }
        }
    }

    double sr = 44100.0;
    int window = 2048, hop = 256, minLag = 36, maxLag = 735, fftSize = 4096;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> ring, lin, nsdf;
    std::vector<std::complex<float>> fa, fb;
    int writeIdx = 0, sinceHop = 0;
    juce::int64 total = 0;
    std::array<PitchFrame, queueSize> queue {};
    int qHead = 0, qCount = 0;
};
} // namespace murmur::dsp
