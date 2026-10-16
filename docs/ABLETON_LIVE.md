# Murmur in Ableton Live

Murmur is an **audio effect** that outputs MIDI. Live 10.1 or later can route a plug-in's
MIDI output to another track ("MIDI From" plug-ins).

## Setup

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
