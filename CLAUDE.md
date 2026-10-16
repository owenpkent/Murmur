# CLAUDE.md - Murmur AI Onboarding

## About the Owner

Owen is a wheelchair user with muscular dystrophy who produces music with a single mouse.
Typing is hard: be proactive, make decisions, offer A/B/C choices so he can answer with one
letter. Mouse-only operability is not a preference here, it is the product. Never use em
dashes in anything you write.

## What This Is

A JUCE VST3 (+ Standalone) **audio effect** that turns a sung, hummed or beatboxed voice into
MIDI. Melody mode emits scale-locked notes; Drums mode emits kick/snare/hat hits for a Drum
Rack. A "dynamic quantize" (strength, swing, grid, free-tempo grid inference when the host is
stopped) tightens the timing. Part of the OK Studio line.

**The difference from every other plugin in the line: it takes AUDIO IN.** It is classified
with `okstudio_add_plugin(... AUDIO_EFFECT)`: not an instrument, not a MIDI effect. Audio
passes through unchanged, MIDI goes out, and `acceptsMidi()` stays true (Ableton refuses a
plugin with no MIDI input bus). In Live it sits on an audio track and a MIDI track picks it
as "MIDI From".

## Build & Run

```powershell
.\build.ps1 -Test        # Release VST3 + Standalone, run unit tests, copy to Ableton
cmake -B build -G "Visual Studio 17 2022" -A x64 -DMURMUR_COPY_PLUGIN=OFF
cmake --build build --config Release --target Murmur_VST3 Murmur_Standalone Murmur_tests
ctest --test-dir build -C Release --output-on-failure
```

Artefacts: `build/Murmur_artefacts/Release/{VST3,Standalone}/`. Local install copies the
`.vst3` to `%USERPROFILE%\Ableton\vst3` (Owen's Ableton custom folder). Needs JUCE at
`../JUCE` and the kit at `../okstudio-juce-kit`.

## Architecture

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first. Load-bearing ideas:

- **Kit-based.** Theme, scales, state, mouse-only contract, updater come from
  `../okstudio-juce-kit`. Fix shared behaviour there. Never call `juce_add_plugin` directly.
- **`src/dsp/` is the engine** (`murmur::dsp::MurmurEngine`, header-only): pitch tracking,
  onset detection, drum classification, quantize. The processor is a thin shell: mono mix in,
  `process`, `drainMidi` out. Engine unit tests live in `tests/DspTests.cpp`.
- **Parameters** go APVTS -> engine as plain floats once per block. Calibration is a
  ValueTree the processor saves next to the APVTS (wrapped in `MURMUR_CALIBRATION`).
- **Threads.** Audio thread: processBlock only. UI thread: 30 Hz timer reading atomic
  engine getters; calibration calls are message-thread.
- **Latency** comes from the engine and is reported via `setLatencySamples`.

## Rules

- Every interaction answers: can this be done with a click, a drag, or a scroll? Hit targets
  at least `okstudio::ui::minHitPx`.
- Use ripgrep (`rg`) for searching, never grep.
- Never add AI attribution to commits or published text.
- Keep docs current as work lands.
