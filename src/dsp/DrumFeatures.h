#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

// DrumFeatures: a small, fixed feature vector describing the first 20 ms after an onset.
//
// Why these features: kick, snare and hat are separated by WHERE the energy sits and HOW
// noisy it is, not by fine spectral detail. Spectral centroid (in log Hz, since pitch
// perception is logarithmic), flatness (tonal thump vs noisy hiss), zero-crossing rate (a
// cheap time-domain brightness cue that survives when the FFT window is short), three
// coarse band-energy ratios (0-300 Hz, 300-2500 Hz, 2500+ Hz) and total level in dB. Seven
// numbers are few enough that a handful of per-user examples pins down a class, which is
// the whole point of the calibration step. Each feature is z-normalised with fixed priors
// so a plain Euclidean distance is meaningful in the classifier.
//
// Upgrade path (rejected for v1): Gist (MFCCs, spectral crest/rolloff) would extend this
// vector with richer timbre descriptors; the classifier is dimension-agnostic apart from
// the weights table.
namespace murmur::dsp
{
struct FeatureVector
{
    static constexpr int size = 7;
    enum Index { CentroidLogHz = 0, Flatness, Zcr, LowRatio, MidRatio, HighRatio, EnergyDb };
    std::array<float, size> v {};
};

namespace drumnorm
{
// Population priors used to z-normalise raw features (hand-chosen from typical beatbox takes).
inline constexpr std::array<float, FeatureVector::size> mean { 7.2f, 0.20f, 0.20f, 0.35f, 0.30f, 0.35f, -25.0f };
inline constexpr std::array<float, FeatureVector::size> stdev { 1.5f, 0.20f, 0.15f, 0.40f, 0.30f, 0.40f, 15.0f };

inline FeatureVector normalise(const std::array<float, FeatureVector::size>& raw)
{
    FeatureVector f;
    for (int i = 0; i < FeatureVector::size; ++i)
        f.v[(size_t) i] = (raw[(size_t) i] - mean[(size_t) i]) / stdev[(size_t) i];
    return f;
}
} // namespace drumnorm

class DrumFeatureExtractor
{
public:
    static constexpr double windowSeconds = 0.020;
    static constexpr double preRollSeconds = 0.003; // start slightly before the reported onset

    void prepare(double sampleRate, int maxBlock)
    {
        juce::ignoreUnused(maxBlock);
        sr = sampleRate;
        windowLen = std::max(64, (int) std::lround(windowSeconds * sr));
        int order = 6;
        while ((1 << order) < windowLen)
            ++order;
        fftN = 1 << order;
        fft = std::make_unique<juce::dsp::FFT>(order);
        buf.assign((size_t) (2 * fftN), 0.0f);
        reset();
    }

    void reset() {}

    int getWindowLength() const { return windowLen; }
    int getPreRollSamples() const { return (int) std::lround(preRollSeconds * sr); }

    // `x` points at windowLen samples. Returns the z-normalised vector.
    FeatureVector extract(const float* x)
    {
        const int n = windowLen;
        std::fill(buf.begin(), buf.end(), 0.0f);
        double energy = 0.0;
        int crossings = 0;
        const int fadeStart = n - n / 10;
        for (int i = 0; i < n; ++i)
        {
            energy += (double) x[i] * x[i];
            if (i > 0 && ((x[i] >= 0.0f) != (x[i - 1] >= 0.0f)))
                ++crossings;
            const float fade = i < fadeStart ? 1.0f : 0.5f * (1.0f + std::cos(juce::MathConstants<float>::pi * (float) (i - fadeStart) / (float) (n - fadeStart)));
            buf[(size_t) i] = x[i] * fade;
        }
        fft->performFrequencyOnlyForwardTransform(buf.data(), true);

        const int bins = fftN / 2 + 1;
        const double binHz = sr / fftN;
        double sumM = 0.0, sumFM = 0.0, logSum = 0.0, pLow = 0.0, pMid = 0.0, pHigh = 0.0;
        for (int k = 1; k < bins; ++k)
        {
            const double m = buf[(size_t) k] + 1.0e-9;
            const double f = k * binHz;
            sumM += m;
            sumFM += f * m;
            logSum += std::log(m);
            const double p = m * m;
            if (f < 300.0) pLow += p;
            else if (f < 2500.0) pMid += p;
            else pHigh += p;
        }
        const double count = bins - 1;
        const double centroid = sumFM / sumM;
        const double flatness = std::exp(logSum / count) / (sumM / count);
        const double pTot = pLow + pMid + pHigh + 1.0e-18;

        std::array<float, FeatureVector::size> raw {};
        raw[FeatureVector::CentroidLogHz] = (float) std::log(std::max(centroid, 20.0));
        raw[FeatureVector::Flatness] = (float) juce::jlimit(0.0, 1.0, flatness);
        raw[FeatureVector::Zcr] = (float) crossings / (float) (n - 1);
        raw[FeatureVector::LowRatio] = (float) (pLow / pTot);
        raw[FeatureVector::MidRatio] = (float) (pMid / pTot);
        raw[FeatureVector::HighRatio] = (float) (pHigh / pTot);
        raw[FeatureVector::EnergyDb] = (float) (10.0 * std::log10(energy / n + 1.0e-12));
        return drumnorm::normalise(raw);
    }

private:
    double sr = 44100.0;
    int windowLen = 882, fftN = 1024;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> buf;
};
} // namespace murmur::dsp
