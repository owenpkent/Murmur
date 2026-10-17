#pragma once

#include "TakeModel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

// TakePlayer: plays a take's notes as MIDI (docs/TAKE_EDITOR_DESIGN.md, Playback).
//
// Thread contract, the same handoff as TakeCapture: publish()/play()/stop() on the message
// thread only; render() on the audio thread, no lock and no allocation. publish() fills the
// spare one of two preallocated snapshots, swaps the active pointer, then waits out any render
// in flight (the in-use flag) so the old slot is free to refill next time.
//
// Two ways to play. Free: the take's own sample clock, from play() until the take ends or
// stop(). Follow host: an anchored take under a playing host, position
// (ppq - anchorPpq) * 60 / anchorBpm. Free wins when both apply. Events are scheduled at take
// time + latency, so the host's delay compensation lines them up with the delayed audio.
//
// Anything that breaks continuity (stop, seek, a jump of more than a block, a new snapshot,
// the take ending) first sends a note-off for everything sounding, tracked in a 128-entry
// table, so no note ever hangs.
namespace murmur::take
{
class TakePlayer
{
public:
    static constexpr int maxNotes = 2048;

    TakePlayer()
    {
        for (auto& s : slots)
            s.notes.resize((size_t) maxNotes);
        events.resize((size_t) maxNotes * 2);
    }

    // Message thread. Builds the playback form of `t` in the spare slot and swaps it in.
    void publish(const Take& t, double sampleRate)
    {
        auto& s = slots[spare];
        s.count = 0;
        s.length = 0;
        if (sampleRate > 0.0)
        {
            for (const auto& n : t.notes)
            {
                if (s.count >= maxNotes)
                    break;
                const auto a = (juce::int64) std::llround(std::max(0.0, n.start) * sampleRate);
                const auto b = std::max(a + 1, (juce::int64) std::llround(n.end * sampleRate));
                const int vel = juce::jlimit(1, 127, 1 + (int) std::lround((double) n.velocity * 126.0));
                s.notes[(size_t) s.count++] = { a, b, juce::jlimit(0, 127, n.targetNote), (juce::uint8) vel };
                s.length = std::max(s.length, b);
            }
            std::stable_sort(s.notes.begin(), s.notes.begin() + s.count,
                             [](const Note& x, const Note& y) { return x.startSample < y.startSample; });
        }
        s.hasAnchor = t.anchorPpq.has_value();
        s.anchorPpq = t.anchorPpq.value_or(0.0);
        s.anchorBpm = t.anchorBpm > 0.0 ? t.anchorBpm : 120.0;
        s.id = ++idCounter;

        active.store(&s);
        spare = (&slots[0] == &s) ? 1 : 0;
        while (inRender.load())
            std::this_thread::yield();
    }

    // Message thread. Free play from `fromSeconds`; a second call restarts it.
    void play(double fromSeconds)
    {
        fromSecs.store(std::max(0.0, fromSeconds));
        playhead.store(std::max(0.0, fromSeconds));
        auto s = state.load();
        while (! state.compare_exchange_weak(s, (((s >> 1) + 1) << 1) | 1u)) {}
    }

    void stop()
    {
        auto s = state.load();
        while (! state.compare_exchange_weak(s, s & ~1u)) {}
    }

    bool isPlaying() const noexcept { return (state.load() & 1u) != 0 || following.load(); }
    double playheadSeconds() const noexcept { return playhead.load(); }

    // Audio thread, right after render() returned true: the take position, in samples, of that
    // block's first sample (the voice reads the corrected take from here).
    juce::int64 blockStart() const noexcept { return blockPos; }

    // Audio thread. Adds this block's events to `out`. Returns true when the take is playing
    // this block, so the caller should use `out` instead of the live MIDI. Note-offs sent while
    // a play ends are in `out` either way.
    bool render(juce::MidiBuffer& out, int numSamples, juce::int64 latency, bool hostPlaying,
                double ppqAtBlockStart, double sampleRate) noexcept
    {
        inRender.store(true);
        const bool tookOver = renderImpl(out, numSamples, latency, hostPlaying, ppqAtBlockStart, sampleRate);
        inRender.store(false);
        return tookOver;
    }

private:
    struct Note
    {
        juce::int64 startSample = 0, endSample = 0;
        int note = 60;
        juce::uint8 velocity = 100;
    };
    struct Slot
    {
        std::vector<Note> notes;
        int count = 0;
        juce::int64 length = 0;
        bool hasAnchor = false;
        double anchorPpq = 0.0, anchorBpm = 120.0;
        int id = 0;
    };
    struct Event
    {
        int offset;
        bool on;
        int note;
        juce::uint8 velocity;
    };
    enum class Mode { idle, free, follow };

    void silence(juce::MidiBuffer& out) noexcept
    {
        if (soundingTotal == 0)
            return;
        for (int n = 0; n < 128; ++n)
            if (sounding[(size_t) n] > 0)
                out.addEvent(juce::MidiMessage::noteOff(1, n), 0);
        sounding.fill(0);
        soundingTotal = 0;
    }

    bool renderImpl(juce::MidiBuffer& out, int numSamples, juce::int64 latency, bool hostPlaying,
                    double ppq, double sr) noexcept
    {
        auto* s = active.load();
        const auto st = state.load();
        const bool freeOn = (st & 1u) != 0;
        bool reset = false;
        if (freeOn && (st >> 1) != seenSerial)
        {
            seenSerial = st >> 1;
            freePos = (juce::int64) (fromSecs.load() * sr);
            reset = true;
        }

        Mode mode = Mode::idle;
        juce::int64 pos = 0;
        if (s != nullptr && sr > 0.0)
        {
            if (freeOn)
            {
                mode = Mode::free;
                pos = freePos;
            }
            else if (s->hasAnchor && hostPlaying)
            {
                pos = (juce::int64) std::llround((ppq - s->anchorPpq) * 60.0 / s->anchorBpm * sr);
                if (pos + numSamples > 0 && pos < s->length + latency)
                    mode = Mode::follow;
            }
        }

        following.store(mode == Mode::follow);

        if (mode == Mode::idle)
        {
            silence(out);
            lastMode = Mode::idle;
            return false;
        }

        blockPos = pos;
        const bool jumped = mode == Mode::follow && std::llabs(pos - expectedPos) > (juce::int64) numSamples;
        if (mode != lastMode || s->id != seenId || reset || jumped)
            silence(out);
        lastMode = mode;
        seenId = s->id;

        // Gather this block's note-ons and note-offs, then play them in time order (offs
        // before ons at the same sample), so a note shorter than the block still pairs up.
        int ne = 0;
        const juce::int64 hi = pos + numSamples;
        for (int i = 0; i < s->count; ++i)
        {
            const auto& n = s->notes[(size_t) i];
            const auto on = n.startSample + latency, off = n.endSample + latency;
            if (on >= pos && on < hi)
                events[(size_t) ne++] = { (int) (on - pos), true, n.note, n.velocity };
            if (off >= pos && off < hi)
                events[(size_t) ne++] = { (int) (off - pos), false, n.note, 0 };
        }
        std::sort(events.begin(), events.begin() + ne, [](const Event& a, const Event& b)
        {
            return a.offset != b.offset ? a.offset < b.offset : (! a.on && b.on);
        });
        for (int i = 0; i < ne; ++i)
        {
            const auto& e = events[(size_t) i];
            auto& c = sounding[(size_t) e.note];
            if (e.on)
            {
                ++c;
                ++soundingTotal;
                out.addEvent(juce::MidiMessage::noteOn(1, e.note, e.velocity), e.offset);
            }
            else if (c > 0)
            {
                --c;
                --soundingTotal;
                if (c == 0)
                    out.addEvent(juce::MidiMessage::noteOff(1, e.note), e.offset);
            }
        }

        expectedPos = hi;
        playhead.store((double) pos / sr);
        if (mode == Mode::free)
        {
            freePos = hi;
            if (hi > s->length + latency)
            {
                auto expected = st;
                state.compare_exchange_strong(expected, st & ~1u); // a newer play() keeps its flag
                lastMode = Mode::idle;
            }
        }
        return true;
    }

    std::array<Slot, 2> slots;
    int spare = 1; // message thread only
    int idCounter = 0; // message thread: stamps each snapshot
    std::atomic<Slot*> active { nullptr };
    std::atomic<bool> inRender { false };

    std::atomic<unsigned> state { 0 }; // bit 0 = free play on, the rest a restart serial
    std::atomic<double> fromSecs { 0.0 };
    std::atomic<double> playhead { 0.0 };
    std::atomic<bool> following { false };

    // Audio thread only.
    unsigned seenSerial = 0;
    int seenId = 0;
    juce::int64 freePos = 0, expectedPos = 0, blockPos = 0;
    Mode lastMode = Mode::idle;
    std::array<int, 128> sounding {};
    int soundingTotal = 0;
    std::vector<Event> events;
};
} // namespace murmur::take
