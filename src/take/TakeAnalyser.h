#pragma once

#include "TakeModel.h"

#include "../dsp/NoteSegmenter.h"
#include "../dsp/OnsetDetector.h"
#include "../dsp/PitchTracker.h"

#include <algorithm>
#include <vector>

// TakeAnalyser: turns a recorded mono buffer into a Take (pitch curve plus notes), offline.
//
// Why a re-run instead of saving the live notes: the same PitchTracker, OnsetDetector and
// NoteSegmenter on the same audio with the same Params give the same notes the singer heard
// live, and the offline pass also yields the whole pitch curve the roll draws. The block
// loop mirrors the Melody path of MurmurEngine::process exactly (onsets reach the segmenter
// before the pitch frames of the same block).
//
// Allocation: this runs on a background thread after Stop, never the audio thread, so it
// allocates freely (the curve, the event list, fresh analysers). The analysers' fixed queues
// cannot overflow at 512-sample blocks: the tracker yields 2 to 3 frames (queue 128), the
// onset detector at most a couple of events (queue 64), and the segmenter a few events
// (queue 64), and all three are drained after every block.
namespace murmur::take
{
inline Take analyse(const float* mono, int numSamples, double sampleRate,
                    const murmur::dsp::NoteSegmenter::Params& params, float onsetSensitivity)
{
    constexpr int block = 512;
    murmur::dsp::PitchTracker tracker;
    murmur::dsp::OnsetDetector onsets;
    murmur::dsp::NoteSegmenter segmenter;
    tracker.prepare(sampleRate, block);
    onsets.prepare(sampleRate, block);
    segmenter.prepare(sampleRate, block);
    onsets.setSensitivity(onsetSensitivity);
    segmenter.params = params;

    Take take;
    take.sampleRate = sampleRate;
    std::vector<murmur::dsp::NoteEvent> events;

    auto drainNotes = [&]
    {
        murmur::dsp::NoteEvent e;
        while (segmenter.pop(e))
            events.push_back(e);
    };

    for (int pos = 0; pos < numSamples; pos += block)
    {
        const int n = std::min(block, numSamples - pos);
        tracker.process(mono + pos, n);
        onsets.process(mono + pos, n);

        murmur::dsp::OnsetEvent o;
        while (onsets.pop(o))
            segmenter.pushOnset(o);
        murmur::dsp::PitchFrame f;
        while (tracker.pop(f))
        {
            const bool voiced = f.f0Hz > 0.0 && f.clarity >= params.voicedOnClarity;
            take.curve.push_back({ (double) f.samplePos / sampleRate,
                                   voiced ? (float) murmur::dsp::PitchTracker::hzToMidi(f.f0Hz) : 0.0f,
                                   f.clarity, f.rms });
            segmenter.pushFrame(f);
        }
        drainNotes();
    }
    segmenter.forceNoteOff(numSamples);
    drainNotes();

    // Pair each On with the Off that follows it.
    const murmur::dsp::NoteEvent* open = nullptr;
    auto close = [&](const murmur::dsp::NoteEvent& off)
    {
        TakeNote n;
        n.start = (double) open->samplePos / sampleRate;
        n.end = (double) off.samplePos / sampleRate;
        n.detectedNote = n.targetNote = open->midiNote;
        n.velocity = open->velocity;
        n.sungMidi = (float) open->midiNote + (float) (open->cents / 100.0);
        std::vector<float> voiced;
        for (const auto& p : take.curve)
            if (p.midi > 0.0f && p.seconds >= n.start && p.seconds < n.end)
                voiced.push_back(p.midi);
        if (! voiced.empty())
        {
            std::nth_element(voiced.begin(), voiced.begin() + (long) voiced.size() / 2, voiced.end());
            n.sungMidi = voiced[voiced.size() / 2];
        }
        take.notes.push_back(n);
        open = nullptr;
    };
    for (const auto& e : events)
    {
        if (e.type == murmur::dsp::NoteEvent::On)
        {
            if (open != nullptr)
                close({ murmur::dsp::NoteEvent::Off, open->midiNote, 0.0f, e.samplePos, 0.0 });
            open = &e;
        }
        else if (open != nullptr)
            close(e);
    }
    return take;
}
} // namespace murmur::take
