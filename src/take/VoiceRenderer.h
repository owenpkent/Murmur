#pragma once

#include "TakeModel.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

// VoiceRenderer: the offline pitch shift behind Phase B of the Take Editor (see
// docs/TAKE_EDITOR_DESIGN.md, "Phase B: the voice").
//
// Why offline and per note: the corrected take is a copy of the recording in which only the
// edited notes are re-rendered through signalsmith-stretch, so an untouched note is never
// processed at all and sounds exactly as sung, and an edit re-renders one note rather than the
// take. Everything here runs on a background thread and may allocate; nothing is audio-thread
// safe and nothing needs to be.
//
// The shift is per analysis frame, not per note, so a moved note keeps its own intonation and
// vibrato: with `moved = frameMidi + (target - detected)` and `out = target + drift * (moved -
// target)` the shift is `out - frameMidi`. Drift 1 keeps the wobble, drift 0 pins the note to
// its centre. Unvoiced frames have no pitch to correct, so they take the plain move.
//
// Latency: the stretcher delays its output by inputLatency() + outputLatency(). outputSeek()
// primes it with that much input and pre-computes the output before it, after which output
// sample i lines up with input sample i (this is what the library's own `exact()` does, here
// fed in short chunks so the shift can follow the curve). The first stretch of that primed
// output is an estimate, so the region is rendered with a lead-in taken from before the note
// (zeros where the take starts), and the lead-in is thrown away. The end is the same: a tail of
// real audio after the note is fed and then flush() drains the stretcher.
namespace murmur::take
{
class VoiceRenderer
{
public:
    static constexpr double padSeconds = 0.030;  // rendered either side of the note
    static constexpr double fadeSeconds = 0.015; // equal-power crossfade at each end of the region
    static constexpr int chunkSamples = 256;

    // Semitones to move the sound at one analysis frame. frameMidi 0 means unvoiced.
    static float shiftAt(const TakeNote& n, float frameMidi)
    {
        const float move = (float) (n.targetNote - n.detectedNote);
        if (frameMidi <= 0.0f)
            return move;
        const float moved = frameMidi + move;
        const float target = (float) n.targetNote;
        const float out = target + n.drift * (moved - target);
        return out - frameMidi;
    }

    // False for a note exactly as sung: nothing to render, the original is used.
    static bool needsRender(const TakeNote& n) { return n.targetNote != n.detectedNote || n.drift < 1.0f; }

    // Re-renders one note's region of `corrected` from `original`. Both buffers are mono at
    // `sampleRate` and the same length. A note that needs no rendering restores the original
    // over its region instead (so reverting an edit undoes it).
    static void renderNote(const std::vector<float>& original, std::vector<float>& corrected, const Take& take,
                           int noteIndex, double sampleRate)
    {
        if (noteIndex < 0 || noteIndex >= (int) take.notes.size())
            return;
        const auto& note = take.notes[(size_t) noteIndex];
        if (needsRender(note))
            renderRegion(original, corrected, take, note, sampleRate);
        else
            restoreRegion(original, corrected, note.start, note.end, sampleRate);
    }

    // The same as renderNote, but always runs the stretcher, even for a 0 shift. For tests.
    static void renderNoteForced(const std::vector<float>& original, std::vector<float>& corrected,
                                 const Take& take, int noteIndex, double sampleRate)
    {
        if (noteIndex < 0 || noteIndex >= (int) take.notes.size())
            return;
        renderRegion(original, corrected, take, take.notes[(size_t) noteIndex], sampleRate);
    }

    // Puts `original` back over [start, end] plus the pad, as a plain copy (the original is
    // what lies outside the region, so there is no seam). A neighbour whose pad overlaps this
    // one should be re-rendered afterwards.
    static void restoreRegion(const std::vector<float>& original, std::vector<float>& corrected, double start,
                              double end, double sampleRate)
    {
        const auto [a, b] = regionOf(start, end, sampleRate, (int) original.size());
        if (b > a && corrected.size() == original.size())
            std::copy(original.begin() + a, original.begin() + b, corrected.begin() + a);
    }

    // A copy of `original` with every note that needs it rendered.
    static std::vector<float> renderAll(const std::vector<float>& original, const Take& take, double sampleRate)
    {
        auto corrected = original;
        for (int i = 0; i < (int) take.notes.size(); ++i)
            if (needsRender(take.notes[(size_t) i]))
                renderNote(original, corrected, take, i, sampleRate);
        return corrected;
    }

    // ---- Incremental rendering --------------------------------------------------------------

    // What a note looked like when it was last rendered.
    struct RenderedNote
    {
        double start = 0.0, end = 0.0;
        int targetNote = 60;
        float drift = 1.0f;
    };

    static std::vector<RenderedNote> snapshot(const Take& take)
    {
        std::vector<RenderedNote> v;
        for (const auto& n : take.notes)
            v.push_back({ n.start, n.end, n.targetNote, n.drift });
        return v;
    }

    // Indices (into take.notes) whose region must be re-rendered: new notes and notes whose
    // target or drift differ from `prev`. Notes are matched by their start and end, which edits
    // never change.
    static std::vector<int> changedNotes(const std::vector<RenderedNote>& prev, const Take& take)
    {
        std::vector<int> out;
        for (int i = 0; i < (int) take.notes.size(); ++i)
        {
            const auto& n = take.notes[(size_t) i];
            const auto it = std::find_if(prev.begin(), prev.end(),
                                         [&](const RenderedNote& p) { return p.start == n.start && p.end == n.end; });
            if (it == prev.end() || it->targetNote != n.targetNote || it->drift != n.drift)
                out.push_back(i);
        }
        return out;
    }

    // Regions of `prev` notes that are gone from the take (erased), whose old render must be
    // undone with restoreRegion().
    static std::vector<std::pair<double, double>> removedRegions(const std::vector<RenderedNote>& prev,
                                                                 const Take& take)
    {
        std::vector<std::pair<double, double>> out;
        for (const auto& p : prev)
        {
            const bool kept = std::any_of(take.notes.begin(), take.notes.end(),
                                          [&](const TakeNote& n) { return p.start == n.start && p.end == n.end; });
            if (! kept)
                out.emplace_back(p.start, p.end);
        }
        return out;
    }

private:
    using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

    // [a, b) sample range of the note plus the pad, clamped to the buffer.
    static std::pair<int, int> regionOf(double start, double end, double sr, int length)
    {
        const int pad = (int) std::lround(padSeconds * sr);
        const int a = std::clamp((int) std::floor(start * sr) - pad, 0, length);
        const int b = std::clamp((int) std::ceil(end * sr) + pad, 0, length);
        return { a, std::max(a, b) };
    }

    // Writes src[0 .. b-a) into dst[a .. b) with an equal-power crossfade at both ends. A region
    // touching the start or end of the buffer has no outside to fade into, so that end is hard.
    static void blendIn(std::vector<float>& dst, const float* src, int a, int b, double sr)
    {
        const int n = b - a;
        const int fade = std::min((int) std::lround(fadeSeconds * sr), n / 2);
        const bool fadeIn = a > 0, fadeOut = b < (int) dst.size();
        const double halfPi = 1.5707963267948966;
        for (int i = 0; i < n; ++i)
        {
            double w = 1.0; // weight of the new audio
            if (fadeIn && i < fade)
                w = std::min(w, (double) i / fade);
            if (fadeOut && i >= n - fade)
                w = std::min(w, (double) (n - 1 - i) / fade);
            const double wn = std::sin(w * halfPi), wo = std::cos(w * halfPi);
            float& d = dst[(size_t) (a + i)];
            d = (float) (wo * d + wn * src[i]);
        }
    }

    // The curve's pitch at `seconds` (nearest point), 0 when unvoiced or the curve is empty.
    static float midiAt(const Take& take, double seconds)
    {
        const auto& c = take.curve;
        if (c.empty())
            return 0.0f;
        auto it = std::lower_bound(c.begin(), c.end(), seconds,
                                   [](const PitchPoint& p, double s) { return p.seconds < s; });
        if (it == c.end())
            return (it - 1)->midi;
        if (it != c.begin() && seconds - (it - 1)->seconds < it->seconds - seconds)
            --it;
        return it->midi;
    }

    static void renderRegion(const std::vector<float>& original, std::vector<float>& corrected, const Take& take,
                             const TakeNote& note, double sr)
    {
        const int total = (int) original.size();
        const auto [a, b] = regionOf(note.start, note.end, sr, total);
        if (b - a < 2 || corrected.size() != original.size())
            return;

        Stretch st;
        st.presetDefault(1, (float) sr);
        st.setFormantFactor(1.0f, true);
        const int seekLen = st.outputSeekLength(1.0);
        const int inLat = st.inputLatency();

        // Extended input: a lead-in before the region and a tail after it, zeros past the ends.
        const int lead = seekLen, tail = seekLen;
        const int ext = lead + (b - a) + tail;
        std::vector<float> in((size_t) ext, 0.0f), out((size_t) ext, 0.0f);
        for (int i = 0; i < ext; ++i)
        {
            const int s = a - lead + i;
            if (s >= 0 && s < total)
                in[(size_t) i] = original[(size_t) s];
        }

        // The shift for the stretcher frame whose centre is at extended-input index `idx`.
        const double noteStart = note.start, noteEnd = note.end;
        auto shiftFor = [&](int idx) {
            const double t = std::clamp((double) (a - lead + idx) / sr, noteStart, noteEnd);
            return shiftAt(note, midiAt(take, t));
        };

        const float* inPtr[1] = { in.data() };
        st.setTransposeSemitones(shiftFor(seekLen - inLat));
        st.outputSeek(inPtr, seekLen);

        int pos = seekLen; // next input sample to feed; output index = pos - seekLen
        while (pos < ext)
        {
            const int n = std::min(chunkSamples, ext - pos);
            // The frame being computed is centred inLat behind the newest input fed.
            st.setTransposeSemitones(shiftFor(pos + n - inLat));
            const float* ip[1] = { in.data() + pos };
            float* op[1] = { out.data() + (pos - seekLen) };
            st.process(ip, n, op, n);
            pos += n;
        }
        float* tailOut[1] = { out.data() + (ext - seekLen) };
        st.flush(tailOut, seekLen, 1.0);

        blendIn(corrected, out.data() + lead, a, b, sr);
    }
};
} // namespace murmur::take
