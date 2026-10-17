#pragma once

#include "TakeModel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <vector>

// TakeMidi: a Take as a standard MIDI file, for the Drag MIDI tile. Pure, so the tests read the
// result back without a processor or a file.
//
// Times are seconds from the take's start, converted with the take's anchorBpm (120 when the
// take is free), so the clip lands on Live's grid when Live runs at the tempo the take was
// sung to. Each note lasts exactly its start to its end (at least one tick), at its
// targetNote, so an edit is what gets exported. The whole file is one tempo track.
namespace murmur::take
{
constexpr int midiFileTicksPerQuarter = 960;

inline juce::MidiFile toMidiFile(const Take& t)
{
    const double bpm = t.anchorPpq.has_value() && t.anchorBpm > 0.0 ? t.anchorBpm : 120.0;
    const double ticksPerSecond = bpm / 60.0 * midiFileTicksPerQuarter;

    struct Event
    {
        int tick;
        bool on;
        int note, velocity;
    };
    std::vector<Event> events;
    for (const auto& n : t.notes)
    {
        const int on = (int) std::llround(juce::jmax(0.0, n.start) * ticksPerSecond);
        const int off = juce::jmax(on + 1, (int) std::llround(juce::jmax(0.0, n.end) * ticksPerSecond));
        const int note = juce::jlimit(0, 127, n.targetNote);
        const int vel = juce::jlimit(1, 127, (int) std::lround(n.velocity * 127.0f));
        events.push_back({ on, true, note, vel });
        events.push_back({ off, false, note, 0 });
    }
    // Note-offs sort before note-ons at the same tick, so a repeated note retriggers cleanly.
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b)
                     { return a.tick != b.tick ? a.tick < b.tick : (! a.on && b.on); });

    juce::MidiMessageSequence seq;
    auto tempo = juce::MidiMessage::tempoMetaEvent((int) std::llround(60000000.0 / bpm));
    tempo.setTimeStamp(0.0);
    seq.addEvent(tempo);

    int lastTick = 0;
    for (const auto& e : events)
    {
        auto m = e.on ? juce::MidiMessage::noteOn(1, e.note, (juce::uint8) e.velocity)
                      : juce::MidiMessage::noteOff(1, e.note);
        m.setTimeStamp((double) e.tick);
        seq.addEvent(m);
        lastTick = juce::jmax(lastTick, e.tick);
    }
    auto end = juce::MidiMessage::endOfTrack();
    end.setTimeStamp((double) lastTick);
    seq.addEvent(end);

    juce::MidiFile file;
    file.setTicksPerQuarterNote(midiFileTicksPerQuarter);
    file.addTrack(seq);
    return file;
}
} // namespace murmur::take
