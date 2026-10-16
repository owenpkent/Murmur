#pragma once

#include "DrumClassifier.h"
#include "DrumFeatures.h"
#include "NoteSegmenter.h"
#include "OnsetDetector.h"
#include "PitchTracker.h"
#include "Quantizer.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// MurmurEngine: the thin facade that wires the analysers to MIDI.
//
// Why a facade: the plugin shell should not know which detectors exist. It hands over mono
// audio and transport info and drains MIDI; everything else (pitch, onsets, classification,
// segmentation, quantize) lives behind this class so each piece stays unit-testable alone.
//
// Latency design: drum classification needs 20 ms of audio AFTER the onset, so a hit cannot
// be known until then. The engine therefore reports getLatencySamples() = 20 ms and every
// event, drum or melody, is released at (event time + latency + quantize shift). The host
// compensates by that amount, so hits land on the beat the singer meant. Melody notes pass
// through the same delay purely so both modes line up; their own detection takes longer
// (the pitch window and the 40 ms stability test), so melody note-ons are stamped back at
// the true start of the note but can only be released when decided, i.e. they come out at
// offset 0 of the block in which they were confirmed whenever that is already past.
//
// Quantize: with the transport playing, host PPQ/BPM define the grid. When stopped, the
// free-tempo GridInference (fed by every onset) defines it if its confidence is >= 0.5.
// Only forward-and-backward shifts that still land inside the current or a future block can
// be honoured; an earlier target is released immediately (a real-time device cannot go
// back in time). Note-offs are shifted by the same amount as their note-on, so length is
// preserved.
//
// Threading: process() and drainMidi() run on the audio thread. The classifier and
// segmenter params are exposed by reference; mutate them from the audio thread or guard
// externally. drainMidi() does not allocate as long as the caller's vector has capacity.
namespace murmur::dsp
{
class MurmurEngine
{
public:
    enum class Mode { Melody, Drums };
    enum class DrumClass { Kick = 0, Snare, HatClosed, HatOpen };

    struct MidiOut
    {
        int sampleOffset = 0;
        juce::MidiMessage msg;
    };

    void prepare(double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        tracker.prepare(sr, maxBlock);
        onsets.prepare(sr, maxBlock);
        segmenter.prepare(sr, maxBlock);
        extractor.prepare(sr, maxBlock);
        classifier.prepare(sr, maxBlock);
        inference.prepare(sr, maxBlock);
        int cap = 1;
        while ((double) cap < sr * 1.0) cap <<= 1;
        audio.assign((size_t) cap, 0.0f);
        audioMask = cap - 1;
        featureBuf.assign((size_t) extractor.getWindowLength(), 0.0f);
        reset();
    }

    void reset()
    {
        tracker.reset();
        onsets.reset();
        segmenter.reset();
        extractor.reset();
        classifier.reset();
        inference.reset();
        std::fill(audio.begin(), audio.end(), 0.0f);
        written = 0;
        blockStart = 0;
        pendingCount = 0;
        outCount = 0;
        lastOnDelta = 0;
        inputLevel.store(0.0f);
        lastNote.store(-1);
        inferredBpm.store(0.0);
        inferredConf.store(0.0f);
    }

    int getLatencySamples() const { return (int) std::lround(DrumFeatureExtractor::windowSeconds * sr); }

    void setMode(Mode m)
    {
        if (m == mode)
            return;
        segmenter.forceNoteOff(written);
        flushSegmenterEvents();
        pendingCount = 0;
        mode = m;
    }
    Mode getMode() const { return mode; }

    void setOnsetSensitivity(float s) { onsets.setSensitivity(s); }

    // ---- Shell-facing setters (plain stores; called from the audio thread before process) ----
    void setSensitivity(float s) { onsets.setSensitivity(s); }
    void setQuantizeStrength(float s) { qStrength = s; }
    void setSwing(float s) { qSwing = s; }
    void setGridDivision(int i) { grid = (GridQuantizer::Grid) juce::jlimit(0, 5, i); }
    void setScale(int root, int scaleIndex)
    {
        segmenter.params.rootPitchClass = root;
        segmenter.params.scaleIndex = scaleIndex;
    }
    void setScaleLock(bool on) { segmenter.params.scaleLock = on; }

    // ---- Calibration (message thread begins/clears/serialises; audio thread adds + classifies) ----
    // The next 3 onsets' feature vectors become examples of `c`, then calibration ends.
    void beginCalibration(DrumClass c)
    {
        const juce::SpinLock::ScopedLockType l(classifierLock);
        calibClass = (int) c;
        calibRemaining.store(calibrationHits);
    }
    bool isCalibrating() const { return calibRemaining.load() > 0; }
    // The class being calibrated, or -1 when no calibration is running.
    int getCalibratingClass() const
    {
        const juce::SpinLock::ScopedLockType l(classifierLock);
        return calibRemaining.load() > 0 ? calibClass : -1;
    }
    // How many user examples a class holds (0 means it still classifies on the built-in defaults).
    int getNumCalibrationExamples(DrumClass c) const
    {
        const juce::SpinLock::ScopedLockType l(classifierLock);
        return classifier.getNumExamples((DrumClassifier::Class) c);
    }
    void clearCalibration(DrumClass c)
    {
        const juce::SpinLock::ScopedLockType l(classifierLock);
        classifier.clearClass((DrumClassifier::Class) c);
    }
    juce::ValueTree calibrationToValueTree() const
    {
        const juce::SpinLock::ScopedLockType l(classifierLock);
        return classifier.toValueTree();
    }
    void calibrationFromValueTree(const juce::ValueTree& t)
    {
        const juce::SpinLock::ScopedLockType l(classifierLock);
        classifier.fromValueTree(t);
    }

    // ---- UI getters (atomics refreshed in process) ----
    float getInputLevel() const { return inputLevel.load(); }
    int getLastNote() const { return lastNote.load(); }
    double getInferredBpm() const { return inferredBpm.load(); }
    float getInferredBpmConfidence() const { return inferredConf.load(); }

    // strength 0 disables; window is in beats.
    void setQuantize(GridQuantizer::Grid g, float strength, float swing, double snapWindowBeats)
    {
        grid = g; qStrength = strength; qSwing = swing; qWindow = snapWindowBeats;
    }

    // Host transport for the block about to be processed.
    void setTransport(double ppqAtBlockStart, double bpm, bool playing)
    {
        ppqStart = ppqAtBlockStart;
        hostBpm = bpm > 1.0 ? bpm : 120.0;
        playingFlag = playing;
    }

    NoteSegmenter::Params& segmenterParams() { return segmenter.params; }
    DrumClassifier& getClassifier() { return classifier; }
    const GridInference& getGridInference() const { return inference; }
    std::array<int, DrumClassifier::numClasses>& drumNotes() { return drumNoteMap; }

    // Last classified hit, for UI feedback and calibration capture.
    struct LastHit { DrumClassifier::Result result; FeatureVector features; bool valid = false; };
    const LastHit& getLastHit() const { return lastHit; }

    // `hostBlockStartSample` is accepted for symmetry with the host API; the engine keeps its
    // own gap-free timeline (positions only matter relative to each other).
    void process(const float* mono, int n, juce::int64 hostBlockStartSample)
    {
        juce::ignoreUnused(hostBlockStartSample);
        blockStart = written;
        for (int i = 0; i < n; ++i)
            audio[(size_t) ((written + i) & audioMask)] = mono[i];
        written += n;

        {
            float pk = 0.0f;
            for (int i = 0; i < n; ++i)
                pk = std::max(pk, std::abs(mono[i]));
            const float db = 20.0f * std::log10(std::max(pk, 1.0e-6f));
            const float lvl = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
            const float prev = inputLevel.load();
            inputLevel.store(lvl > prev ? lvl : prev * 0.85f + lvl * 0.15f); // fast attack, slow release
        }

        tracker.process(mono, n);
        onsets.process(mono, n);

        OnsetEvent o;
        while (onsets.pop(o))
        {
            inference.addOnset((double) o.samplePos / sr);
            if (mode == Mode::Melody)
                segmenter.pushOnset(o);
            else
            {
                if (pendingCount == maxPending) // drop oldest
                {
                    std::copy(pending.begin() + 1, pending.end(), pending.begin());
                    --pendingCount;
                }
                pending[(size_t) pendingCount++] = o;
            }
        }
        PitchFrame f;
        while (tracker.pop(f))
            if (mode == Mode::Melody)
                segmenter.pushFrame(f);

        if (mode == Mode::Drums)
            resolveDrums();
        else
            flushSegmenterEvents();

        if (inference.hasEstimate())
        {
            inferredBpm.store(inference.get().bpm);
            inferredConf.store(inference.get().confidence);
        }
    }

    // Appends all events due in the block just processed. Offsets are relative to its start.
    void drainMidi(std::vector<MidiOut>& dest)
    {
        for (;;)
        {
            int best = -1;
            for (int i = 0; i < outCount; ++i)
                if (out[(size_t) i].due < written && (best < 0 || out[(size_t) i].due < out[(size_t) best].due))
                    best = i;
            if (best < 0)
                break;
            const auto& e = out[(size_t) best];
            dest.push_back({ (int) juce::jmax<juce::int64>(0, e.due - blockStart), e.msg });
            for (int i = best; i + 1 < outCount; ++i)
                out[(size_t) i] = out[(size_t) (i + 1)];
            --outCount;
        }
    }

private:
    static constexpr int maxPending = 16;
    static constexpr int maxOut = 256;

    struct Queued { juce::int64 due = 0; juce::MidiMessage msg; };

    void queue(juce::int64 due, const juce::MidiMessage& m)
    {
        if (outCount == maxOut) // drop oldest
        {
            std::copy(out.begin() + 1, out.end(), out.begin());
            --outCount;
        }
        out[(size_t) outCount++] = { due, m };
    }

    // Sample shift that moves an event at `pos` onto the grid (0 when quantize is off/unknown).
    juce::int64 quantDelta(juce::int64 pos) const
    {
        if (qStrength <= 0.0f)
            return 0;
        double beats, bpm;
        if (playingFlag)
        {
            bpm = hostBpm;
            beats = ppqStart + (double) (pos - blockStart) / sr * bpm / 60.0;
        }
        else if (inference.hasEstimate() && inference.get().confidence >= 0.5f)
        {
            bpm = inference.get().bpm;
            beats = inference.secondsToBeats((double) pos / sr);
        }
        else
            return 0;
        const double q = GridQuantizer::quantize(beats, grid, qStrength, qSwing, qWindow);
        return (juce::int64) std::llround((q - beats) * 60.0 / bpm * sr);
    }

    void flushSegmenterEvents()
    {
        std::array<NoteEvent, 16> evs {};
        int n = 0;
        NoteEvent e;
        while (n < (int) evs.size() && segmenter.pop(e))
            evs[(size_t) n++] = e;
        const juce::int64 lat = getLatencySamples();
        for (int i = 0; i < n; ++i)
        {
            const auto& ev = evs[(size_t) i];
            if (ev.type == NoteEvent::On)
            {
                lastOnDelta = quantDelta(ev.samplePos);
                lastNote.store(ev.midiNote);
                queue(ev.samplePos + lat + lastOnDelta,
                      juce::MidiMessage::noteOn(1, ev.midiNote, (juce::uint8) juce::jlimit(1, 127, (int) std::lround(ev.velocity * 127.0f))));
            }
            else
            {
                // An Off directly followed by an On (retrigger) shares the On's shift so
                // the pair cannot cross over.
                const bool nextIsOn = i + 1 < n && evs[(size_t) (i + 1)].type == NoteEvent::On;
                const auto delta = nextIsOn ? quantDelta(evs[(size_t) (i + 1)].samplePos) : lastOnDelta;
                queue(ev.samplePos + lat + delta, juce::MidiMessage::noteOff(1, ev.midiNote));
            }
        }
    }

    void resolveDrums()
    {
        const int len = extractor.getWindowLength();
        const int pre = extractor.getPreRollSamples();
        const juce::int64 lat = getLatencySamples();
        int keep = 0;
        for (int i = 0; i < pendingCount; ++i)
        {
            const auto& o = pending[(size_t) i];
            const juce::int64 start = o.samplePos - pre;
            if (written < start + len || start < 0)
            {
                if (start >= 0)
                    pending[(size_t) keep++] = o; // not enough audio yet
                continue;
            }
            for (int j = 0; j < len; ++j)
                featureBuf[(size_t) j] = audio[(size_t) ((start + j) & audioMask)];
            const auto feat = extractor.extract(featureBuf.data());
            DrumClassifier::Result res;
            {
                // Never block the audio thread: if the message thread holds the lock, drop this hit.
                const juce::SpinLock::ScopedTryLockType l(classifierLock);
                if (! l.isLocked())
                    continue;
                const int rem = calibRemaining.load();
                if (rem > 0)
                {
                    classifier.addExample((DrumClassifier::Class) calibClass, feat);
                    calibRemaining.store(rem - 1);
                }
                res = classifier.classify(feat);
            }
            lastHit = { res, feat, true };

            const auto& p = segmenter.params;
            const double db = 20.0 * std::log10(std::max((double) o.peakRms, 1.0e-6));
            const float vel = juce::jlimit(0.05f, 1.0f, ((float) db - p.velFloorDb) / std::max(1.0f, p.velCeilDb - p.velFloorDb));
            const int note = drumNoteMap[(size_t) res.cls];
            lastNote.store(note);
            const auto delta = quantDelta(o.samplePos);
            const auto on = o.samplePos + lat + delta;
            queue(on, juce::MidiMessage::noteOn(1, note, (juce::uint8) juce::jlimit(1, 127, (int) std::lround(vel * 127.0f))));
            queue(on + (juce::int64) std::lround(0.060 * sr), juce::MidiMessage::noteOff(1, note));
        }
        pendingCount = keep;
    }

    double sr = 44100.0;
    Mode mode = Mode::Melody;
    PitchTracker tracker;
    OnsetDetector onsets;
    NoteSegmenter segmenter;
    DrumFeatureExtractor extractor;
    DrumClassifier classifier;
    GridInference inference;

    std::vector<float> audio, featureBuf;
    int audioMask = 0;
    juce::int64 written = 0, blockStart = 0, lastOnDelta = 0;

    std::array<OnsetEvent, maxPending> pending {};
    int pendingCount = 0;
    std::array<Queued, maxOut> out {};
    int outCount = 0;

    std::array<int, DrumClassifier::numClasses> drumNoteMap { 36, 38, 42, 46 };
    LastHit lastHit;

    static constexpr int calibrationHits = 3;
    mutable juce::SpinLock classifierLock;
    int calibClass = 0; // guarded by classifierLock
    std::atomic<int> calibRemaining { 0 };
    std::atomic<float> inputLevel { 0.0f };
    std::atomic<int> lastNote { -1 };
    std::atomic<double> inferredBpm { 0.0 };
    std::atomic<float> inferredConf { 0.0f };

    GridQuantizer::Grid grid = GridQuantizer::Grid::Sixteenth;
    float qStrength = 0.0f, qSwing = 0.0f;
    double qWindow = 0.25;
    double ppqStart = 0.0, hostBpm = 120.0;
    bool playingFlag = false;
};
} // namespace murmur::dsp
