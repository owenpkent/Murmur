// Take Editor, Drag MIDI (docs/TAKE_EDITOR_DESIGN.md, step 5): the pure Take -> MIDI file
// conversion, read back the way Live would read the file.
// tests/DspTests.cpp owns the runner's main(); this file is self-registering statics only.

#include "../src/take/TakeMidi.h"

#include <juce_core/juce_core.h>

namespace murmur::tests
{
namespace
{
    take::TakeNote note(double start, double end, int target, float vel = 0.5f)
    {
        take::TakeNote n;
        n.start = start;
        n.end = end;
        n.detectedNote = 60;
        n.targetNote = target;
        n.velocity = vel;
        return n;
    }

    // The file written to bytes and parsed again, so the tests see what a DAW would.
    juce::MidiFile roundTrip(const take::Take& t)
    {
        juce::MemoryOutputStream out;
        take::toMidiFile(t).writeTo(out);
        juce::MemoryInputStream in(out.getData(), out.getDataSize(), false);
        juce::MidiFile f;
        f.readFrom(in);
        return f;
    }

    struct Ev
    {
        int tick;
        bool on;
        int note, velocity;
    };

    std::vector<Ev> noteEvents(const juce::MidiFile& f)
    {
        std::vector<Ev> v;
        if (const auto* track = f.getTrack(0))
            for (auto* e : *track)
                if (e->message.isNoteOnOrOff())
                    v.push_back({ (int) e->message.getTimeStamp(), e->message.isNoteOn(),
                                  e->message.getNoteNumber(), (int) e->message.getVelocity() });
        return v;
    }

    // The seconds-per-quarter of the first tempo event, or 0 when there is none.
    double tempoSecondsPerQuarter(const juce::MidiFile& f)
    {
        if (const auto* track = f.getTrack(0))
            for (auto* e : *track)
                if (e->message.isTempoMetaEvent())
                    return e->message.getTempoSecondsPerQuarterNote();
        return 0.0;
    }

    int endOfTrackTick(const juce::MidiFile& f)
    {
        if (const auto* track = f.getTrack(0))
            for (auto* e : *track)
                if (e->message.isEndOfTrackMetaEvent())
                    return (int) e->message.getTimeStamp();
        return -1;
    }
} // namespace

class TakeMidiTests : public juce::UnitTest
{
public:
    TakeMidiTests() : juce::UnitTest("Take MIDI export", "murmur") {}

    void runTest() override
    {
        beginTest("a free take: 960 ticks per quarter, 120 BPM, exact note times");
        {
            take::Take t;
            t.notes = { note(0.0, 0.5, 60, 1.0f), note(0.5, 1.5, 64, 0.5f) };
            const auto f = roundTrip(t);
            expectEquals((int) f.getTimeFormat(), 960);
            expectEquals(f.getNumTracks(), 1);
            expectWithinAbsoluteError(tempoSecondsPerQuarter(f), 0.5, 1e-5);

            // 120 BPM is 2 quarters a second, so 1920 ticks a second.
            const auto ev = noteEvents(f);
            expectEquals((int) ev.size(), 4);
            if (ev.size() == 4)
            {
                expect(ev[0].on && ev[0].tick == 0 && ev[0].note == 60 && ev[0].velocity == 127, "note 1 on");
                expect(! ev[1].on && ev[1].tick == 960 && ev[1].note == 60, "note 1 off at 0.5 s");
                expect(ev[2].on && ev[2].tick == 960 && ev[2].note == 64 && ev[2].velocity == 64, "note 2 on");
                expect(! ev[3].on && ev[3].tick == 2880 && ev[3].note == 64, "note 2 off at 1.5 s");
            }
            expectEquals(endOfTrackTick(f), 2880);
        }

        beginTest("an anchored take uses its own tempo: 90 BPM");
        {
            take::Take t;
            t.anchorPpq = 16.0;
            t.anchorBpm = 90.0;
            t.notes = { note(1.0, 2.0, 67) };
            const auto f = roundTrip(t);
            expectWithinAbsoluteError(tempoSecondsPerQuarter(f), 60.0 / 90.0, 1e-5);

            // 90 BPM is 1.5 quarters a second, so 1440 ticks a second.
            const auto ev = noteEvents(f);
            expectEquals((int) ev.size(), 2);
            if (ev.size() == 2)
            {
                expectEquals(ev[0].tick, 1440);
                expectEquals(ev[1].tick, 2880);
            }
            expectEquals(endOfTrackTick(f), 2880);
        }

        beginTest("an edited note exports its target, not what was heard");
        {
            take::Take t;
            t.notes = { note(0.0, 0.5, 60), note(0.5, 1.0, 62) };
            t.notes[1].detectedNote = 62;
            t.notes[1].targetNote = 65;
            const auto ev = noteEvents(roundTrip(t));
            expectEquals((int) ev.size(), 4);
            if (ev.size() == 4)
            {
                expectEquals(ev[0].note, 60);
                expectEquals(ev[2].note, 65);
                expectEquals(ev[3].note, 65);
            }
        }

        beginTest("at an equal time a note-off comes before a note-on");
        {
            take::Take t;
            t.notes = { note(0.0, 0.5, 60), note(0.5, 1.0, 60) }; // the same pitch, back to back
            const auto ev = noteEvents(roundTrip(t));
            expectEquals((int) ev.size(), 4);
            if (ev.size() == 4)
            {
                expect(ev[1].tick == 960 && ev[2].tick == 960, "both at 0.5 s");
                expect(! ev[1].on && ev[2].on, "off first, then on");
            }
        }

        beginTest("a note never has zero length, and velocity is at least 1");
        {
            take::Take t;
            t.notes = { note(0.25, 0.25, 60, 0.0f) };
            const auto ev = noteEvents(roundTrip(t));
            expectEquals((int) ev.size(), 2);
            if (ev.size() == 2)
            {
                expect(ev[1].tick > ev[0].tick, "off after on");
                expect(ev[0].velocity >= 1, "audible velocity");
            }
        }

        beginTest("an empty take gives a file with a tempo and no notes");
        {
            const auto f = roundTrip(take::Take {});
            expectEquals(f.getNumTracks(), 1);
            expectEquals((int) noteEvents(f).size(), 0);
            expectWithinAbsoluteError(tempoSecondsPerQuarter(f), 0.5, 1e-5);
            expectEquals(endOfTrackTick(f), 0);
        }
    }
};

static TakeMidiTests takeMidiTests;
} // namespace murmur::tests
