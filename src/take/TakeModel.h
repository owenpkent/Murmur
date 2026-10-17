#pragma once

#include <okstudio/Scales.h>

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// TakeModel: the data of one recorded take and the pure edits on it.
//
// Why pure functions on a plain struct: the editor, the MCP tools and the tests all make the
// same edits, and a take holds at most a few hundred notes, so an undo entry is simply a copy
// of `notes`. Nothing here touches audio, threads or the UI, which keeps every rule below
// unit-testable without a processor. See docs/TAKE_EDITOR_DESIGN.md.
//
// `curve` is the sung pitch over time and is large (a point per 256 samples), so it is never
// saved or put on the undo stack: it is recomputed from the WAV by TakeAnalyser.
// `targetNote` is the only thing an edit changes; `detectedNote` stays as what was heard, so
// Reset can always go back. Times are seconds from the start of the take.
namespace murmur::take
{
struct PitchPoint
{
    double seconds = 0.0;
    float midi = 0.0f; // fractional MIDI note; 0 = unvoiced
    float clarity = 0.0f;
    float rms = 0.0f;
};

struct TakeNote
{
    double start = 0.0, end = 0.0;
    float sungMidi = 0.0f;  // median of the voiced curve inside the note
    int detectedNote = 60;  // what the live segmenter chose
    int targetNote = 60;    // what it plays as
    float velocity = 0.0f;  // 0..1
    float drift = 1.0f;     // Phase B: 1 keeps the sung wobble, 0 flattens it
};

struct Take
{
    double sampleRate = 0.0;
    std::vector<PitchPoint> curve;
    std::vector<TakeNote> notes;
    std::optional<double> anchorPpq;
    double anchorBpm = 120.0;
    juce::String audioPath;

    bool isEmpty() const { return notes.empty() && curve.empty() && audioPath.isEmpty(); }
};

// ---- Edits -------------------------------------------------------------------------------

namespace detail
{
    // The next in-scale note strictly above (dir +1) or below (dir -1) `n`, or `n` at the edge.
    inline int nextInScale(int n, int dir, int root, int scaleIndex)
    {
        for (int m = n + dir; m >= 0 && m <= 127; m += dir)
            if (okstudio::scales::isInScale(m, root, scaleIndex))
                return m;
        return n;
    }
} // namespace detail

// Moves a note's target by `steps` semitones, or by `steps` scale degrees when scaleLock is
// on (a target that is off the scale first steps onto its nearest legal neighbour).
inline bool shiftNote(Take& t, int index, int steps, bool scaleLock, int root, int scaleIndex)
{
    if (index < 0 || index >= (int) t.notes.size())
        return false;
    auto& n = t.notes[(size_t) index];
    if (! scaleLock)
    {
        n.targetNote = juce::jlimit(0, 127, n.targetNote + steps);
        return true;
    }
    int v = n.targetNote;
    for (int i = 0; i < std::abs(steps); ++i)
        v = detail::nextInScale(v, steps > 0 ? 1 : -1, root, scaleIndex);
    n.targetNote = v;
    return true;
}

inline void snapAllToScale(Take& t, int root, int scaleIndex)
{
    for (auto& n : t.notes)
        n.targetNote = juce::jlimit(0, 127, okstudio::scales::snapToScale(n.targetNote, root, scaleIndex));
}

inline bool resetNote(Take& t, int index)
{
    if (index < 0 || index >= (int) t.notes.size())
        return false;
    auto& n = t.notes[(size_t) index];
    n.targetNote = n.detectedNote;
    n.drift = 1.0f;
    return true;
}

inline bool deleteNote(Take& t, int index)
{
    if (index < 0 || index >= (int) t.notes.size())
        return false;
    t.notes.erase(t.notes.begin() + index);
    return true;
}

// Hit test for the roll: the note drawn in lane `midiLane` (its targetNote) that covers
// `seconds`. The last of any overlapping candidates wins (drawn on top). -1 when none.
inline int noteAt(const Take& t, double seconds, int midiLane)
{
    for (int i = (int) t.notes.size() - 1; i >= 0; --i)
    {
        const auto& n = t.notes[(size_t) i];
        if (n.targetNote == midiLane && seconds >= n.start && seconds < n.end)
            return i;
    }
    return -1;
}

// ---- Undo --------------------------------------------------------------------------------

// Snapshots of `notes` only. Call push() with the take BEFORE an edit (once per gesture).
class TakeHistory
{
public:
    static constexpr size_t maxEntries = 100;

    void push(const Take& t)
    {
        stack.push_back(t.notes);
        if (stack.size() > maxEntries)
            stack.erase(stack.begin());
    }

    bool undo(Take& t)
    {
        if (stack.empty())
            return false;
        t.notes = std::move(stack.back());
        stack.pop_back();
        return true;
    }

    bool canUndo() const { return ! stack.empty(); }
    void clear() { stack.clear(); }

private:
    std::vector<std::vector<TakeNote>> stack;
};

// ---- State -------------------------------------------------------------------------------

inline juce::ValueTree toValueTree(const Take& t)
{
    juce::ValueTree v("MURMUR_TAKE");
    v.setProperty("sampleRate", t.sampleRate, nullptr);
    if (t.anchorPpq.has_value())
        v.setProperty("anchor", *t.anchorPpq, nullptr);
    v.setProperty("anchorBpm", t.anchorBpm, nullptr);
    v.setProperty("audioPath", t.audioPath, nullptr);
    for (const auto& n : t.notes)
    {
        juce::ValueTree c("NOTE");
        c.setProperty("start", n.start, nullptr);
        c.setProperty("end", n.end, nullptr);
        c.setProperty("sungMidi", (double) n.sungMidi, nullptr);
        c.setProperty("detectedNote", n.detectedNote, nullptr);
        c.setProperty("targetNote", n.targetNote, nullptr);
        c.setProperty("velocity", (double) n.velocity, nullptr);
        c.setProperty("drift", (double) n.drift, nullptr);
        v.appendChild(c, nullptr);
    }
    return v;
}

// An invalid or wrong-typed tree gives an empty Take.
inline Take fromValueTree(const juce::ValueTree& v)
{
    Take t;
    if (! v.isValid() || ! v.hasType("MURMUR_TAKE"))
        return t;
    t.sampleRate = v.getProperty("sampleRate", 0.0);
    if (v.hasProperty("anchor"))
        t.anchorPpq = (double) v.getProperty("anchor");
    t.anchorBpm = v.getProperty("anchorBpm", 120.0);
    t.audioPath = v.getProperty("audioPath", juce::String()).toString();
    for (const auto& c : v)
    {
        if (! c.hasType("NOTE"))
            continue;
        TakeNote n;
        n.start = c.getProperty("start", 0.0);
        n.end = c.getProperty("end", 0.0);
        n.sungMidi = (float) (double) c.getProperty("sungMidi", 0.0);
        n.detectedNote = c.getProperty("detectedNote", 60);
        n.targetNote = c.getProperty("targetNote", n.detectedNote);
        n.velocity = (float) (double) c.getProperty("velocity", 0.0);
        n.drift = (float) (double) c.getProperty("drift", 1.0);
        t.notes.push_back(n);
    }
    return t;
}
} // namespace murmur::take
