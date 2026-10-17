#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

// OnsetDetector: SuperFlux-style spectral flux onsets.
//
// Why this algorithm: "la la la" on one pitch has no pitch change to split the notes, and
// beatboxing has no pitch at all, so both modes need a note/hit START detector that works
// on the spectrum. Spectral flux (how much each frequency bin grew since the last frame)
// is the standard answer. Two refinements keep it honest on voices: (1) log-compressing
// the magnitudes so quiet consonants count as much as loud vowels, and (2) comparing each
// bin against the MAX of its three neighbours in the previous frame (SuperFlux), which
// stops vibrato, whose partials sweep sideways across bins, from reading as a stream of
// onsets. The adaptive threshold is the median of the last ~0.5 s of flux scaled by the
// sensitivity, plus a floor; a peak must also stand clear of the recent local average so
// sustained noise (an open hat "tss") does not retrigger itself.
//
// Upgrade path (rejected for v1): aubio's onset module (complex-domain / specflux modes) or
// a small learned onset model are drop-in replacements behind the same pop() API.
//
// Timing: `samplePos` is the centre of the FFT frame holding the flux peak. A peak is only
// confirmed one hop later, so events arrive about 1.5 frames (~4 ms) after samplePos.
namespace murmur::dsp
{
struct OnsetEvent
{
    juce::int64 samplePos = 0;
    float strength = 0.0f; // 0..1, how far the flux peak stood above the threshold
    float peakRms = 0.0f;  // RMS of the FFT frame at the peak
};

class OnsetDetector
{
public:
    static constexpr int fftOrder = 9;
    static constexpr int fftSize = 1 << fftOrder; // 512
    static constexpr int hop = 128;
    static constexpr int numBins = fftSize / 2 + 1;

    // 0 = conservative, 1 = very sensitive.
    void setSensitivity(float s) { sensitivity = juce::jlimit(0.0f, 1.0f, s); }

    void prepare(double sampleRate, int maxBlock)
    {
        juce::ignoreUnused(maxBlock);
        sr = sampleRate;
        fft = std::make_unique<juce::dsp::FFT>(fftOrder);
        for (int i = 0; i < fftSize; ++i)
            win[(size_t) i] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * (float) i / (float) fftSize);
        histLen = std::max(8, (int) std::lround(0.5 * sr / hop));
        refractory = (juce::int64) std::lround(0.040 * sr);
        hist.assign((size_t) histLen, 0.0f);
        scratch.assign((size_t) histLen, 0.0f);
        reset();
    }

    void reset()
    {
        ring.fill(0.0f);
        prevLog.fill(0.0f);
        fluxRecent.fill(0.0f);
        rmsRecent.fill(0.0f);
        std::fill(hist.begin(), hist.end(), 0.0f);
        histIdx = 0;
        writeIdx = 0;
        total = 0;
        sinceHop = 0;
        lastOnset = -1000000000;
        qHead = qCount = 0;
    }

    void process(const float* in, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) writeIdx] = in[i];
            writeIdx = (writeIdx + 1) % fftSize;
            ++total;
            if (++sinceHop >= hop)
            {
                sinceHop = 0;
                if (total >= fftSize)
                    frame();
            }
        }
    }

    bool pop(OnsetEvent& out)
    {
        if (qCount == 0)
            return false;
        out = queue[(size_t) qHead];
        qHead = (qHead + 1) % queueSize;
        --qCount;
        return true;
    }

private:
    static constexpr int queueSize = 64;
    static constexpr int recentLen = 10; // fluxRecent[0] is the newest
    static constexpr float magGain = 300.0f;

    void frame()
    {
        double e = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            const float v = ring[(size_t) ((writeIdx + i) % fftSize)];
            e += (double) v * v;
            fftBuf[(size_t) i] = v * win[(size_t) i];
        }
        std::fill(fftBuf.begin() + fftSize, fftBuf.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform(fftBuf.data(), true);

        // Normalise so a unit-amplitude sinusoid reads ~1 (Hann sum is N/2, bin holds half).
        const float norm = 2.0f / (0.5f * (float) fftSize);
        std::array<float, numBins> cur {};
        for (int k = 0; k < numBins; ++k)
            cur[(size_t) k] = std::log1p(magGain * norm * fftBuf[(size_t) k]);

        float flux = 0.0f;
        for (int k = 0; k < numBins; ++k)
        {
            float m = prevLog[(size_t) k];
            if (k > 0) m = std::max(m, prevLog[(size_t) (k - 1)]);
            if (k + 1 < numBins) m = std::max(m, prevLog[(size_t) (k + 1)]);
            flux += std::max(0.0f, cur[(size_t) k] - m);
        }
        prevLog = cur;

        // Shift recent histories (newest at index 0).
        std::rotate(fluxRecent.rbegin(), fluxRecent.rbegin() + 1, fluxRecent.rend());
        fluxRecent[0] = flux;
        std::rotate(rmsRecent.rbegin(), rmsRecent.rbegin() + 1, rmsRecent.rend());
        rmsRecent[0] = (float) std::sqrt(e / fftSize);

        hist[(size_t) histIdx] = flux;
        histIdx = (histIdx + 1) % histLen;

        // Median of the history window.
        std::copy(hist.begin(), hist.end(), scratch.begin());
        auto mid = scratch.begin() + histLen / 2;
        std::nth_element(scratch.begin(), mid, scratch.end());
        const float median = *mid;

        const float mult = 3.0f + (1.3f - 3.0f) * sensitivity;
        const float floorV = 5.0f + (1.2f - 5.0f) * sensitivity; // flux units: summed log-magnitude rise
        const float thresh = median * mult + floorV;

        // Candidate is the previous frame (index 1): needs the next frame to confirm a peak.
        const float c = fluxRecent[1];
        bool isPeak = c > fluxRecent[0];
        for (int j = 2; j <= 4 && isPeak; ++j)
            isPeak = c >= fluxRecent[(size_t) j];
        if (!isPeak || c <= thresh)
            return;
        float localMean = 0.0f;
        for (int j = 2; j < recentLen; ++j)
            localMean += fluxRecent[(size_t) j];
        localMean /= (float) (recentLen - 2);
        if (c <= 1.5f * localMean + floorV)
            return;

        const juce::int64 pos = total - fftSize / 2 - hop; // centre of the candidate frame
        if (pos - lastOnset < refractory)
            return;
        lastOnset = pos;

        OnsetEvent ev;
        ev.samplePos = pos;
        ev.strength = juce::jlimit(0.0f, 1.0f, (c - thresh) / std::max(c, 1.0e-6f));
        ev.peakRms = rmsRecent[1];
        if (qCount == queueSize)
        {
            qHead = (qHead + 1) % queueSize;
            --qCount;
        }
        queue[(size_t) ((qHead + qCount) % queueSize)] = ev;
        ++qCount;
    }

    double sr = 44100.0;
    float sensitivity = 0.5f;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::array<float, fftSize> win {};
    std::array<float, fftSize> ring {};
    std::array<float, 2 * fftSize> fftBuf {};
    std::array<float, numBins> prevLog {};
    std::array<float, recentLen> fluxRecent {};
    std::array<float, recentLen> rmsRecent {};
    std::vector<float> hist, scratch;
    int histLen = 172, histIdx = 0, writeIdx = 0, sinceHop = 0;
    juce::int64 total = 0, lastOnset = -1000000000, refractory = 1764;
    std::array<OnsetEvent, queueSize> queue {};
    int qHead = 0, qCount = 0;
};
} // namespace murmur::dsp
