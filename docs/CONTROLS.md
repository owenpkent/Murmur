# Controls

Every control is operated with a single left-click, a drag, or the scroll wheel. Nothing
needs the keyboard, a double-click, a right-click, or a modifier key, and there are no
exceptions to that. Every target is at least `okstudio::ui::minHitPx` high (the editor uses
that plus 10 px). The plugin never takes keyboard focus from the DAW.

The editor is resizable by dragging its corner (680 x 500 up to 1800 x 1200, 780 x 560 on
open).

## Always visible

| Control | Parameter id | Gesture | Result |
|---------|--------------|---------|--------|
| **Melody / Drums** toggle (the big one) | `mode` | Click a segment | Switches what Murmur turns the voice into: scale-locked notes (Melody) or kick/snare/hat hits (Drums). The controls below that apply to only one mode swap in and out. |
| **Sensitivity** knob | `sensitivity` | Drag, or scroll over it | How easily an onset triggers (0..100%, default 50%). |
| **Quantize** knob | `quantize` | Drag, or scroll over it | How hard events snap to the grid: 0% keeps your timing, 100% snaps fully. Default 70%. |
| **Swing** knob | `swing` | Drag, or scroll over it | Delays every second grid point, by up to a third of a grid step at 100%. Default 0%. |
| **Grid** segmented control | `grid` | Click a segment | Grid division: 1/4, 1/8, 1/16, 1/8T, 1/16T, 1/32. Default 1/16. |
| **Input** level bar | none | Display only | Live input level, fast attack and slow release. |
| **Last note / Last hit** readout | none | Display only | The last note (Melody) or drum hit (Drums) Murmur emitted, or "none". |
| **Free tempo** readout | none | Display only | Shown only while the host transport is stopped: the tempo Murmur infers from the spacing of your onsets ("Free tempo: 120 BPM", or "listening" until it has one). Blank while the host plays, because the grid then follows the host. |
| **Update to vX.Y.Z** button | none | Click | Appears only when a newer signed release exists on `okstudio1/murmur-releases`. Downloads, verifies and runs the installer. |

## Melody mode only

| Control | Parameter id | Gesture | Result |
|---------|--------------|---------|--------|
| **Root** box | `root` | Click to open, click a note | Tonic of the scale (C, C#, ...). |
| **Scale** box | `scale` | Click to open, click a scale | The scale notes are locked to (the kit's scale list, Major first). |
| **Scale Lock** toggle | `scaleLock` | Click | On (default): every note is snapped into the scale, so Murmur never emits a wrong note. Off: the detected pitch is used as it is. |

## Drums mode only

| Control | Gesture | Result |
|---------|---------|--------|
| **Kick / Snare / Closed Hat / Open Hat** buttons | Click one, then make that sound three times into the mic | The next three onsets become examples of that class (the button lights up and the hint reads "Make the Kick sound 3 times"). Calibration then ends by itself. |
| **Clear** (one under each class) | Click | Throws away your examples for that class. It falls back to the built-in defaults, which will not suit most voices. |

Output notes are fixed General MIDI Drum Rack notes: kick 36, snare 38, closed hat 42, open
hat 46.

## Driving it without the mouse

The same controls are reachable from Claude Code through the embedded MCP server. That is an
additional way in and not a replacement: see [MCP.md](MCP.md).

## Murmur Host only

Murmur Host has every control above, plus a strip under them. Same gestures, same targets.

| Control | Parameter id | Gesture | Result |
|---------|--------------|---------|--------|
| **Mix** knob | `mix` | Drag, or scroll over it | Crossfades the dry voice and the hosted instrument: 0% is the voice only, 100% is the instrument only. Default 100%. |
| **Instrument** list | none | Click to open, click an instrument | Loads that instrument VST3 (instruments only, VST3 only). The list is what the last Scan found. |
| **Scan** button | none | Click | Looks through the VST3 folders (`C:\Program Files\Common Files\VST3` and `%USERPROFILE%\Ableton\vst3`) on a background thread and lists the instruments found. The list is remembered between sessions. |
| **Hide GUI / Show GUI** button | none | Click | Shows or hides the instrument's own editor below the strip. |
| **Eject** button | none | Click | Unloads the instrument; the voice then passes through. |

The status line under the list says what is playing, or "Instrument not found" if a saved
instrument is missing.
