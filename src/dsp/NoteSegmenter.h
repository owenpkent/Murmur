#pragma once

#include "OnsetDetector.h"
#include "PitchTracker.h"

#include <okstudio/Scales.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>

// NoteSegmenter: turns a stream of pitch frames and onset events into discrete notes.
//
// Why this algorithm: raw f0 is a wobbly curve, not a score. Singers scoop into notes,
// drift, vibrate, and breathe between phrases, so we need a small state machine rather than
// "round every frame". The rules are the usual causal ones: hysteresis on clarity decides
// voiced vs unvoiced without chattering; a 5-frame median on f0 throws out single-frame
// octave glitches; a note only starts once the pitch has stayed inside 50 cents for 40 ms
// (so the scoop up to a note does not spray passing notes); a held note retriggers when
// the pitch moves more than 60 cents and stays there, or when the onset detector reports a
// fresh attack on the same pitch (so "la la la" gives three notes); it ends after 80 ms of
// silence; and nothing is shorter than 60 ms. Note-on is back-dated to the start of the
// stable run, so the stamped time is when the singer actually began the note, not when we
// became sure of it. Everything is causal: no look-ahead beyond the 40 ms stability test.
//
// Upgrade path (rejected for v1): an HMM / Viterbi note tracker (as in pYIN) is more accurate
// offline but needs look-ahead; Basic Pitch style neural transcription is available in the
// kit for non-real-time use.
//
// Velocity is 0..1, mapped from the loudest frame RMS in the stable run between velFloorDb
// and velCeilDb. `cents` is the deviation of the sung pitch from the emitted note (before
// any scale snapping), useful for pitch bend or UI.
namespace murmur::dsp
{
struct NoteEvent
{
    enum Type { On, Off };
    Type type = On;
    int midiNote = 60;
    float velocity = 0.0f;
    juce::int64 samplePos = 0;
    double cents = 0.0;
};

class NoteSegmenter
{
public:
    struct Params
    {
        float voicedOnClarity = 0.6f;
        float voicedOffClarity = 0.4f;
        float stableCents = 50.0f;     // pitch must stay within this to start a note
        float stableMs = 40.0f;        // ... for this long
        float retriggerCents = 60.0f;  // pitch must move further than this to retrigger
        float retriggerHoldMs = 40.0f; // ... and hold for this long
        float unvoicedOffMs = 80.0f;   // silence needed before note-off
        float minNoteMs = 60.0f;
        float velFloorDb = -50.0f;
        float velCeilDb = -10.0f;
        bool scaleLock = false;
        int rootPitchClass = 0; // 0 = C
        int scaleIndex = 0;     // index into okstudio::scales::all()
    };

    Params params;

    void prepare(double sampleRate, int maxBlock)
    {
        juce::ignoreUnused(maxBlock);
        sr = sampleRate;
        reset();
    }

    void reset()
    {
        voiced = false;
        histCount = 0;
            histIdx = 0;
        histIdx = 0;
        state = State::Idle;
        runStart = -1;
        candStart = -1;
        unvoicedStart = -1;
        minRunPos = -1;
        qHead = qCount = 0;
        lastFramePos = 0;
    }

    void pushOnset(const OnsetEvent& o)
    {
        if (state == State::Active)
        {
            if (o.samplePos - noteOnPos >= ms(params.minNoteMs))
            {
                emit(NoteEvent::Off, curNote, 0.0f, o.samplePos, 0.0);
                state = State::Idle;
                runStart = -1;
                minRunPos = o.samplePos - hopEstimate;
            }
        }
        else
        {
            runStart = -1;
            minRunPos = std::max(minRunPos, o.samplePos - hopEstimate);
        }
    }

    void pushFrame(const PitchFrame& f)
    {
        if (lastFramePos > 0 && f.samplePos > lastFramePos)
            hopEstimate = f.samplePos - lastFramePos;
        lastFramePos = f.samplePos;

        // Voicing hysteresis.
        const bool wasVoiced = voiced;
        if (! voiced && f.clarity >= params.voicedOnClarity && f.f0Hz > 0.0)
            voiced = true;
        else if (voiced && (f.clarity < params.voicedOffClarity || f.f0Hz <= 0.0))
            voiced = false;

        if (! voiced)
        {
            histCount = 0;
            histIdx = 0;
            runStart = -1;
            candStart = -1;
            if (state == State::Active)
            {
                if (unvoicedStart < 0)
                    unvoicedStart = f.samplePos;
                if (f.samplePos - unvoicedStart >= ms(params.unvoicedOffMs))
                {
                    const auto offPos = std::max(unvoicedStart, noteOnPos + ms(params.minNoteMs));
                    emit(NoteEvent::Off, curNote, 0.0f, offPos, 0.0);
                    state = State::Idle;
                }
            }
            return;
        }
        if (! wasVoiced) // new voiced run
        {
            histCount = 0;
            histIdx = 0;
        }
        unvoicedStart = -1;

        // 5-frame median of fractional MIDI pitch.
        hist[(size_t) histIdx] = PitchTracker::hzToMidi(f.f0Hz);
        histIdx = (histIdx + 1) % medianLen;
        histCount = std::min(histCount + 1, medianLen);
        std::array<double, medianLen> tmp {};
        std::copy(hist.begin(), hist.begin() + histCount, tmp.begin());
        std::sort(tmp.begin(), tmp.begin() + histCount);
        const double m = tmp[(size_t) (histCount / 2)];
        const double rmsDb = 20.0 * std::log10(std::max((double) f.rms, 1.0e-6));

        if (state == State::Idle)
        {
            if (f.samplePos < minRunPos)
                return;
            if (runStart < 0 || std::abs(m - runRef) * 100.0 > params.stableCents)
            {
                runStart = f.samplePos;
                runRef = m;
                runRmsDb = rmsDb;
            }
            else
                runRmsDb = std::max(runRmsDb, rmsDb);

            if (f.samplePos - runStart >= ms(params.stableMs))
                startNote(m, runStart, runRmsDb);
        }
        else
        {
            if (std::abs(m - noteRef) * 100.0 <= params.retriggerCents)
            {
                candStart = -1;
                return;
            }
            if (candStart < 0 || std::abs(m - candRef) * 100.0 > params.stableCents)
            {
                candStart = f.samplePos;
                candRef = m;
                candRmsDb = rmsDb;
            }
            else
                candRmsDb = std::max(candRmsDb, rmsDb);

            if (f.samplePos - candStart >= ms(params.retriggerHoldMs)
                && candStart - noteOnPos >= ms(params.minNoteMs))
            {
                if (snap(candRef) == curNote)
                {
                    noteRef = candRef; // same legal note after scale lock: just follow the drift
                    candStart = -1;
                    return;
                }
                const auto at = candStart;
                emit(NoteEvent::Off, curNote, 0.0f, at, 0.0);
                startNote(candRef, at, candRmsDb);
            }
        }
    }

    // Ends any held note (mode switch, transport stop, plugin bypass).
    void forceNoteOff(juce::int64 pos)
    {
        if (state == State::Active)
        {
            emit(NoteEvent::Off, curNote, 0.0f, std::max(pos, noteOnPos), 0.0);
            state = State::Idle;
        }
        runStart = -1;
    }

    bool isNoteActive() const { return state == State::Active; }

    bool pop(NoteEvent& out)
    {
        if (qCount == 0)
            return false;
        out = queue[(size_t) qHead];
        qHead = (qHead + 1) % queueSize;
        --qCount;
        return true;
    }

private:
    enum class State { Idle, Active };
    static constexpr int medianLen = 5;
    static constexpr int queueSize = 64;

    juce::int64 ms(float v) const { return (juce::int64) std::lround(v * 0.001 * sr); }

    int snap(double m) const
    {
        const int n = (int) std::lround(m);
        return params.scaleLock ? okstudio::scales::snapToScale(n, params.rootPitchClass, params.scaleIndex) : n;
    }

    void startNote(double m, juce::int64 at, double peakDb)
    {
        curNote = juce::jlimit(0, 127, snap(m));
        noteRef = m;
        noteOnPos = at;
        const float span = std::max(1.0f, params.velCeilDb - params.velFloorDb);
        const float vel = juce::jlimit(0.01f, 1.0f, ((float) peakDb - params.velFloorDb) / span);
        emit(NoteEvent::On, curNote, vel, at, (m - curNote) * 100.0);
        state = State::Active;
        runStart = -1;
        candStart = -1;
        unvoicedStart = -1;
    }

    void emit(NoteEvent::Type t, int note, float vel, juce::int64 pos, double cents)
    {
        if (qCount == queueSize)
        {
            qHead = (qHead + 1) % queueSize;
            --qCount;
        }
        queue[(size_t) ((qHead + qCount) % queueSize)] = { t, note, vel, pos, cents };
        ++qCount;
    }

    double sr = 44100.0;
    State state = State::Idle;
    bool voiced = false;
    std::array<double, medianLen> hist {};
    int histCount = 0, histIdx = 0;
    juce::int64 runStart = -1, candStart = -1, unvoicedStart = -1, minRunPos = -1, noteOnPos = 0;
    juce::int64 lastFramePos = 0, hopEstimate = 256;
    double runRef = 0.0, runRmsDb = -120.0, candRef = 0.0, candRmsDb = -120.0, noteRef = 0.0;
    int curNote = 60;
    std::array<NoteEvent, queueSize> queue {};
    int qHead = 0, qCount = 0;
};
} // namespace murmur::dsp
