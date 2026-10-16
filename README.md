# Murmur

Hum a melody or beatbox a groove and get MIDI. Murmur is a VST3 audio effect in the
OK Studio line: it listens to a vocal track, turns what you sing into notes or drum hits,
and quantizes them so the result sits in time. It is built mouse-only: every control is a
click, a drag, or a scroll. No keyboard, no right-click, no tiny targets.

## What it does

- **Melody mode.** Sing or hum, get scale-locked MIDI notes. Pick a root and a scale and
  Murmur never emits a wrong note.
- **Drums mode.** Beatbox kick, snare, closed hat and open hat. Murmur learns your voice:
  press Calibrate, make the sound three times, done. Output is General MIDI Drum Rack notes
  (kick 36, snare 38, closed hat 42, open hat 46).
- **Dynamic quantize.** Strength (how hard hits snap to the grid), swing, and a grid
  division (1/4 to 1/32, with triplets). When the host is stopped there is no grid to
  follow, so Murmur infers a free tempo from your timing and shows it ("Free tempo: 120 BPM").
- **Murmur Host hosts the instrument for you.** Voice in, drums or synth out, on one track
  ([docs/MURMUR_HOST_DESIGN.md](docs/MURMUR_HOST_DESIGN.md)).
- **Audio passes through unchanged** (plain Murmur). The plugin reports its latency to the host so the
  MIDI lines up.

## Use it in Ableton Live

**One track: Murmur Host.** Drop **Murmur Host** on the vocal audio track, press **Scan**, pick a
drum sampler or synth, and sing: the track plays the instrument. It hosts one instrument VST3
inside itself, so there is no second track. **Mix** blends the voice back in.

**Two tracks: plain Murmur.** 
Put Murmur on the vocal audio track. On a MIDI track with a Drum Rack (or any synth), set
**MIDI From** to that audio track and choose **Murmur** as the source. Needs Live 10.1+.
Full steps, with the gotchas: [docs/ABLETON_LIVE.md](docs/ABLETON_LIVE.md).

## Driving it with Claude

Murmur embeds an MCP server, so Claude Code (or any local MCP client) can read what the
voice analysis is hearing, set the mode, key and quantize, read the inferred tempo, and run or
clear a drum calibration. See [docs/MCP.md](docs/MCP.md).

## Installing

Grab `MurmurSetup-x.y.z.exe` from the Releases page of `okstudio1/murmur-releases` and run it:
it installs Murmur.vst3 (the installer does not ship Murmur Host yet; build it from source) where DAWs look (`C:\Program Files\Common Files\VST3`). Or build from
source. There is no release yet (see [docs/RELEASE.md](docs/RELEASE.md)).

## Accessibility

This is the product, not a feature. Every interaction works with a single left-click, a
drag, or a scroll. Nothing requires the keyboard, a double-click, a modifier key, or a
precise gesture. Hit targets are at least 34 px. The plugin never takes keyboard focus from
your DAW. The contract is shared across the line via the kit's `MouseOnly.h`.

## Build

Needs JUCE at `..\JUCE` and the kit at `..\okstudio-juce-kit` (sibling checkouts), plus
Visual Studio 2022 Build Tools and CMake.

```powershell
.\build.ps1          # Release Murmur + Murmur Host (VST3 + Standalone), copies both VST3s to %USERPROFILE%\Ableton\vst3
.\build.ps1 -Test    # also build and run the unit tests
py run.py            # build and launch Murmur Host standalone (sing into the mic, hear the instrument)
py run.py --murmur   # plain Murmur standalone (MIDI only)
```

Details: [docs/BUILD.md](docs/BUILD.md). How it works: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
Why it is built this way: [docs/DESIGN.md](docs/DESIGN.md), with the sources in
[docs/REFERENCES.md](docs/REFERENCES.md) and the dated log in [docs/DECISIONS.md](docs/DECISIONS.md).
Every control and its gesture: [docs/CONTROLS.md](docs/CONTROLS.md). Cutting a release:
[docs/RELEASE.md](docs/RELEASE.md).

## Status

Pre-0.1. Parameter layout may still change and break saved sessions (noted in the
[changelog](CHANGELOG.md)).

## License

GPL-3.0, see [LICENSE](LICENSE). Compiled binaries link JUCE, so distributing them must also
satisfy JUCE's terms (AGPLv3, a purchased license, or JUCE's free tier conditions).
