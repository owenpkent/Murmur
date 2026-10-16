# Murmur in Ableton Live

Murmur is an **audio effect** that outputs MIDI. Live 10.1 or later can route a plug-in's
MIDI output to another track ("MIDI From" plug-ins).

**Status: not yet load-tested in Live**, for either plugin. The routing below follows Live's
documented "MIDI From" behaviour; plugin classification, the audio-in/MIDI-out bus layout and
the routing itself are the things only a hands-on pass can prove.

There are two ways to use it:

- **Murmur Host, one track (recommended).** Murmur Host hosts one instrument VST3 (a drum
  sampler or synth) inside itself, so voice in, drums or synth out happens on a single audio
  track. See [One track: Murmur Host](#one-track-murmur-host).
- **Murmur, two tracks.** Plain Murmur only emits MIDI, so a second MIDI track must take it as
  "MIDI From". That is the Setup section below. It is the only variant that needs "MIDI From".

## Setup (two tracks, plain Murmur)

1. Build or install Murmur (`.\build.ps1` copies the VST3 to `%USERPROFILE%\Ableton\vst3`).
   In Live: Preferences > Plug-ins > use the VST3 custom folder, then rescan.
2. On the **vocal audio track**, drop Murmur from Plug-ins > VST3. Record-arm or monitor the
   vocal as usual. Audio passes through unchanged.
3. Create a **MIDI track** with a Drum Rack (for Drums mode) or any instrument (Melody mode).
4. On the MIDI track, set **MIDI From** to the vocal audio track. In the second dropdown
   choose **Murmur**.
5. Set the MIDI track's monitor to **In** so you hear the instrument while you sing.

## Drum Rack notes

Drums mode sends General MIDI drum notes, which land on the default Drum Rack pads:

| Sound | Note | Pad |
|-------|------|-----|
| Kick | 36 | C1 |
| Snare | 38 | D1 |
| Closed hat | 42 | F#1 |
| Open hat | 46 | A#1 |

Drag your own samples onto those pads.

## Calibrating

In Drums mode press Kick, Snare, Closed Hat or Open Hat, then make that sound three times.
The button lights while Murmur listens. Clear resets one class. Calibration is saved with
the Live set.

## Gotchas

- **Use headphones.** If the vocal bleeds from speakers into the mic, the drums trigger
  themselves.
- **Latency.** Murmur reports it; if the MIDI feels late, check the track's delay
  compensation is on.
- **Stopped transport.** With the transport stopped there is no grid, so Murmur infers a
  free tempo from your timing (shown top right) and quantizes to that.
- **Not a MIDI effect.** A third-party VST3 MIDI effect cannot sit before an instrument in
  Live, which is why Murmur is an audio effect routed through "MIDI From".

## One track: Murmur Host

Murmur Host is still an audio effect (it needs the voice audio in), but it plays the MIDI it
makes through an instrument VST3 loaded inside it, and outputs that instrument's sound.

1. Build or install Murmur Host (`.\build.ps1` copies `Murmur Host.vst3` next to `Murmur.vst3`
   in `%USERPROFILE%\Ableton\vst3`). Rescan.
2. On the **vocal audio track**, drop **Murmur Host** from Plug-ins > VST3.
3. In its window press **Scan** once, then choose a drum sampler or synth in the **Instrument**
   list. The instrument's own GUI appears below Murmur's controls.
4. Sing. The track now plays the instrument. **Mix** blends the dry voice back in: 100% is the
   instrument only (default), 0% is the voice only.

No second track and no "MIDI From" are needed. The MIDI is still sent out of the track too, so a
second track can take "MIDI From: Murmur Host" if you want it to drive something else as well.
The instrument and its settings are saved with the Live set. If the instrument is later
missing, the window says "Instrument not found" and the voice keeps passing through.
Latency is Murmur's plus the instrument's, reported to Live for delay compensation.
Not yet load-tested in Live (this needs a hands-on pass: plugin classification and that Live
keeps feeding the track audio while the hosted instrument sounds). Hosting a third-party
instrument has not been tried either; the unit tests cover only the no-instrument case.
