# Take Editor: design

**Status: Phase A done (2026-10-17): all six steps (model, capture and analysis, editor UI, MIDI playback, Drag MIDI, MCP tools). Phase B designed, not started.**

## What it is

A Melodyne-style editor inside Murmur. You sing a take, Murmur shows every note it heard as a
blob on a pitch-by-time roll with your actual pitch curve drawn through it, and you drag a blob
up or down to fix it. Owen's ask: "something like melodyne where you can shift pitch. maybe
related to lattice". He picked option C: build it in two phases on one editor.

- **Phase A: the edit changes the MIDI.** The take plays back as MIDI (to the hosted
  instrument in Murmur Host, or out of the track in plain Murmur). Wrong notes are fixed
  before they reach the instrument. No audio is changed. A take can also be dragged out of
  the editor into Live as a `.mid` file.
- **Phase B: the edit changes the voice.** The same drag re-pitches the recorded vocal
  itself, and a per-note Drift control keeps or flattens the wobble. Adds a pitch-shift
  library. The editor does not change; a second output appears under it.

It lives in `MurmurProcessor`, so plain Murmur and Murmur Host both have it. Nothing in it is
gated on `MURMUR_HOST`.

## How it relates to Lattice

Borrowed rules, not borrowed code. Lattice's `RollCanvas` (`../Lattice/src/ui/RollCanvas.*`) is
2400 lines built on a tick grid of whole-semitone cells; a sung note has a fractional pitch, a
curve, and a start time in seconds, so its model does not fit. What carries over is Lattice's
interaction doctrine, which is the part that matters for a single mouse:

- **No handles.** The target is always the whole blob body. The verb is a mode button.
- **Lanes never shrink below `okstudio::ui::minHitPx`.** Pitch zoom is set by the user, never a
  quotient of the window size.
- **A hint strip** says what the cursor is over and what a click would do.
- **The wheel scrolls time** with no modifier.
- **Undo is a button**, one entry per gesture.

The roll uses the mainstream (Melodyne and Lattice's Vertical) orientation: pitch rises up the
left keyboard, time runs left to right. A sung line spans about two octaves and a take runs for
minutes, so time needs the wide axis.

## Model: `src/take/TakeModel.h` (header-only, pure, unit-tested)

```
struct PitchPoint { double seconds; float midi; float clarity; float rms; }   // midi 0 = unvoiced
struct TakeNote
{
    double start, end;      // seconds into the take
    float sungMidi;         // median of the voiced curve inside the note (fractional)
    int detectedNote;       // what the live segmenter chose (scale-snapped if Scale Lock was on)
    int targetNote;         // what it plays as; starts equal to detectedNote; the edit
    float velocity;         // 0..1
    float drift = 1.0f;     // Phase B: 1 keeps the sung wobble, 0 flattens it to targetNote
}
struct Take { double sampleRate; std::vector<PitchPoint> curve; std::vector<TakeNote> notes;
              std::optional<double> anchorPpq; double anchorBpm; juce::File audioFile; }
```

Edits are pure functions on `Take` (`shiftNote`, `snapAllToScale`, `resetNote`, `deleteNote`)
so the undo stack is a vector of `notes` snapshots: a take has at most a few hundred notes.

## Capture: Record and Stop

- **Record** (message thread) allocates a mono float buffer for up to 3 minutes at the current
  sample rate (about 35 MB at 48 kHz) and hands it to the audio thread through an atomic
  pointer, the TakeRecorder pattern. The audio thread copies the same mono mix the engine
  already gets. No allocation or lock on the audio thread. At the cap, recording stops itself.
- If the host transport is playing when the first sample lands, the take records its
  `anchorPpq` and tempo, so it plays back in sync with the song. Otherwise it is free (the
  Standalone always is).
- **Stop** hands the buffer back. A background thread then:
  1. **Analyses** it by running a fresh `PitchTracker`, `OnsetDetector` and `NoteSegmenter`
     over the whole buffer offline, with the same parameters the live engine had. The same code
     on the same audio gives the same notes the singer heard live, plus the full pitch curve.
     Basic Pitch (already in the kit) is a later option for a cleaner pass; it is not needed to
     start.
  2. **Saves** the audio as a 32-bit float WAV in `%APPDATA%\OK Studio\Murmur\Takes\`.
- Plugin state stores the take's notes, edits, anchor and WAV path, never the audio itself
  (minutes of audio in a Live set's plugin chunk would make every save slow). A missing WAV is
  reported and the notes still load, as with a missing Host instrument.

## Playback (Phase A: MIDI)

- While a take is **playing**, the take's notes replace the live engine's MIDI. Live analysis
  keeps running for the level bar, but singing along does not add notes.
- **Anchored take, host playing:** the take position follows the host,
  `(ppq - anchorPpq) * 60 / bpm` seconds. One tempo is assumed (the one recorded); tempo
  changes inside a take are not followed in v1.
- **Free take, or host stopped:** the editor's Play and Stop buttons run the take's own clock
  from the start, or from where the time ruler was clicked.
- Events are emitted one plugin latency late, so the host's delay compensation lines them up
  with the audio, exactly as live events are.
- The note list the audio thread reads is an immutable snapshot published by atomic pointer
  swap. The message thread keeps old snapshots alive until the audio thread has moved off them.
  The audio thread never allocates, locks or frees.

## The editor

A **Live | Edit** segmented toggle at the top of `MurmurEditor` swaps the knob view for the take
view. In Murmur Host the instrument strip and instrument GUI stay below, so you hear the edit
on the real sound.

Take view, top to bottom:

1. **Transport row:** Record, Stop, Play, Undo, Snap all to scale, Clear take, and a **Drag
   MIDI** tile you press and drag into Live's arrangement to drop the take as a clip.
2. **Mode row:** Move (drag a blob up or down), Reset (click a blob to put it back to what was
   sung), Erase (click a blob to delete it). Phase B adds Drift.
3. **The roll:** left keyboard, time ruler on top, blobs drawn at `targetNote` with length from
   start to end, the sung curve drawn through each blob, offset by the same shift so it moves
   with it. The original pitch shows as a faint ghost while a blob is moved.
4. **Hint strip.**

Gestures:

| Gesture | Result |
|---------|--------|
| Move mode, drag a blob up or down | Shifts it by whole semitones as the pointer crosses lanes. With Scale Lock on, it steps through scale notes only. One undo entry per drag. |
| Reset mode, click a blob | `targetNote` back to `detectedNote` (Phase B also drift back to 1). |
| Erase mode, click a blob | Deletes the note. |
| Scroll wheel over the roll | Scrolls time. |
| Scroll wheel over the keyboard | Scrolls pitch. |
| Zoom buttons (+ / - / Fit) | Time zoom, and Fit frames the whole take. |
| Click the time ruler | Sets the free-play start point. |

Lanes are `minHitPx` tall. On Stop, the view opens fitted to the take's pitch range.

## Phase B: the voice

- **Library: signalsmith-stretch** (MIT, header-only, built for vocals, has formant
  compensation). Vendored under `third_party/` with the source commit noted. Rubber Band is
  rejected: GPL or a paid licence. A hand-written PSOLA is possible but would take longer to
  make sound as good.
- **Offline render, not real time.** After an edit, a background thread re-renders only the
  notes that changed (plus a short crossfade either side) into a corrected copy of the take.
  Per analysis frame the shift is `target - sung`, where the target curve is
  `targetNote + drift * (frameMidi - sungMidi)`; the shift is fed to the stretcher in short
  chunks.
- **Playback output** is the corrected take instead of the live input. In Murmur Host it becomes
  the "dry" side of Mix, so Mix still crossfades voice and instrument.
- **Invariant change, on purpose:** "audio passes through untouched" becomes "audio passes
  through untouched unless a take is playing". `ARCHITECTURE.md` and `CLAUDE.md` are updated
  in the same change.
- **Drift mode:** drag a blob left or right to set its drift from 0 to 100%.

## Build order

Phase A, each step testable on its own:

1. `TakeModel.h` and offline analysis (`TakeAnalyser.h`), with unit tests: a synthetic sung
   melody gives the expected notes, edits and undo behave, state round-trips.
2. Capture in `MurmurProcessor`: Record and Stop, the 3-minute buffer, the WAV save, state.
3. The roll and the take view (Live | Edit toggle), Move, Reset and Erase, undo.
4. MIDI playback, free then anchored.
5. Drag MIDI out.
6. MCP tools (`get_take`, `edit_take`, `take_transport`) so Claude can read, edit, record and
   play a take, and docs. (`edit_take` with `op: "shift"` replaced the planned `shift_note`.)

Then Phase B: vendor the library, render, playback output, Drift.

## Open questions, decided by default

- **Take count:** one take at a time in v1. Recording again replaces it (Undo cannot bring it
  back, so Record asks for a confirm click when a take with edits exists).
- **Drums mode:** the take view is Melody only in v1. A drum take (move hits between kick and
  snare) is a natural later addition on the same roll.
- **Timing edits** (dragging a blob along time) are left out of v1. Melodyne has them; pitch
  was the ask.
