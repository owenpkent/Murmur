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
stopped) tightens the timing. Part of the OK Studio line (Undertow = bass, Beatform = drums,
Keys = the played keyboard, Murmur = the voice).

**Murmur Host** (`src/host/`, second target `MurmurHost`) is the one-track variant: it hosts one
instrument VST3 inside itself and outputs its audio. See `docs/MURMUR_HOST_DESIGN.md`.

**The difference from every other plugin in the line: it takes AUDIO IN.** It is classified
with `okstudio_add_plugin(... AUDIO_EFFECT)`: not an instrument, not a MIDI effect. Audio
passes through unchanged, MIDI goes out, and `acceptsMidi()` stays true (Ableton refuses a
plugin with no MIDI input bus). In Live it sits on an audio track and a MIDI track picks it
as "MIDI From".

## Build & Run

**Default dev loop: `run.py`.** Do not send Owen to Ableton to try a change. It closes the
running Standalone, builds exactly one Standalone target (Murmur Host by default, `--murmur`
for plain Murmur), and relaunches it; the Standalone opens on the machine's audio input, so
singing into the mic shows the analysis with no DAW, and with an instrument loaded you hear it.
Smart App Control is enforced here and dev builds are unsigned, so the *launch* of a freshly
linked exe can be held while Windows vets it; `run.py` waits that out with a counter on
screen (`docs/BUILD.md`).

**Owen runs it by double-clicking `run.py` in Explorer** (no arguments, no terminal, the
console holds open on failure). Never tell him to type a command when he could click the file
instead. `run.ps1` is a thin shim over `run.py`, so there is one copy of the logic.

```powershell
py run.py              # build + launch the Murmur Host Standalone
py run.py --murmur     # plain Murmur Standalone
py run.py --no-build   # just relaunch what is already built
```

Go to a real Live load test only for what the Standalone cannot show: bus layout, plugin
classification, "MIDI From" routing, installer, updater, host automation.

Full build and test:

```powershell
.\build.ps1 -Test        # Release VST3 + Standalone, run unit tests, copy to Ableton
cmake -B build -G "Visual Studio 17 2022" -A x64 -DMURMUR_COPY_PLUGIN=OFF
cmake --build build --config Release --target Murmur_VST3 MurmurHost_VST3 Murmur_Standalone MurmurHost_Standalone Murmur_tests
ctest --test-dir build -C Release --output-on-failure
```

Artefacts: `build/Murmur_artefacts/Release/{VST3,Standalone}/` and `build/MurmurHost_artefacts/...`. Note the cmake snippet passes
`-DMURMUR_COPY_PLUGIN=OFF`, so it builds but does **not** install to the DAW folder; if Owen
reports "the VST didn't update", run `build.ps1` (it copies the `.vst3` to
`%USERPROFILE%\Ableton\vst3`, his Ableton custom folder; `%USERPROFILE%\VST3` is NOT scanned)
and have him rescan. Needs JUCE at `../JUCE` and the kit at `../okstudio-juce-kit`.

**Releasing:** double-click `release.py` (it builds, signs with the EV eToken, and verifies the
installer). It must be Owen's own session because the token's PIN prompt is a GUI dialog an
agent cannot answer. The checklist and the four silent-failure contracts are in
`docs/RELEASE.md`. The version lives only in `project(Murmur VERSION ...)`.

## Architecture

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first for how the pieces fit, and
[`docs/DECISIONS.md`](docs/DECISIONS.md) for why they fit that way (the dated log). What
follows is the map of what is true today.

- **Kit-based.** Theme, scales, state, mouse-only contract, updater, MCP server come from
  `../okstudio-juce-kit`. Fix shared behaviour there. Never call `juce_add_plugin` directly.
- **`src/dsp/` is the engine** (`murmur::dsp::MurmurEngine`, header-only): pitch tracking,
  onset detection, drum classification, quantize. The processor is a thin shell: mono mix in,
  `process`, `drainMidi` out. Engine unit tests live in `tests/DspTests.cpp`.
- **Parameters** go APVTS -> engine as plain floats once per block. Calibration is a
  ValueTree the processor saves next to the APVTS (wrapped in `MURMUR_CALIBRATION`).
- **Threads.** Audio thread: processBlock only. UI thread: 30 Hz timer reading atomic engine
  getters; calibration calls are message-thread. MCP tool bodies run on the message thread too.
- **Latency** comes from the engine and is reported via `setLatencySamples`.
- **MCP.** `src/mcp/MurmurMcp.*` registers tools on the kit's `okstudio::mcp::Server`; the
  processor owns one bridge (`processor.mcp()`); the `murmur-mcp` shim is built by
  `okstudio_add_mcp_shim`. Tools: `docs/MCP.md`.

### The files

```
src/PluginProcessor.*   thin shell: APVTS, state, host transport, owns the MCP bridge
src/PluginEditor.*      mouse-only editor, 30 Hz timer, updater button
src/dsp/                MurmurEngine, PitchTracker, OnsetDetector, NoteSegmenter,
                        DrumFeatures, DrumClassifier, Quantizer (header-only)
src/ui/                 Segmented (mode / grid toggles), LevelBar
src/mcp/                MurmurMcp (the MCP tool table)
src/host/               MurmurHostProcessor / MurmurHostEditor (Murmur Host target only)
tests/                  DspTests (owns main), StateTests, McpTests, HostTests
installer/              murmur.iss, signing-thumbprint.txt, generate_brand.py, wizard images
assets/                 Murmur.svg (master), Murmur.ico, logo-1024.png
scripts/                capture-window.ps1 (docs screenshots), downloads.ps1
docs/                   ARCHITECTURE, BUILD, CONTROLS, DECISIONS, DESIGN, MCP, REFERENCES,
                        RELEASE, ABLETON_LIVE, MURMUR_HOST_DESIGN
```

## Invariants (don't break)

- **Audio passes through untouched** in plain Murmur, and in Murmur Host when no instrument is
  loaded. `MurmurProcessor::processBlock` only reads the buffer.
- **Murmur Host: load/eject only on the message thread, bracketed by `suspendProcessing`.** The
  audio thread only reads `instrument`. `MURMUR_HOST` gates only `createPluginFilter`; the tests
  link `MurmurHost`, so keep host-only behaviour out of other `#if`s. The MIDI buffer is
  cleared first because Murmur authors the whole outgoing stream.
- **`acceptsMidi()` stays true** even though nothing is expected on it: Live refuses a plugin
  with no MIDI input bus, and only a Live load test shows it.
- **No allocation on the audio thread after `prepareToPlay`.** `drainMidi` relies on the
  caller's vector having capacity.
- **Releases repo, asset name and tag are pinned** (`okstudio1/murmur-releases`,
  `MurmurSetup-<version>.exe`, `v<version>`). Each fails silently if wrong; see
  `docs/RELEASE.md`.
- **The version has one source**: `project(Murmur VERSION ...)`.
- **The MCP bridge is destroyed first** in the processor destructor, before the members its
  tool bodies read.
- **A bad MCP argument is an error, never a clamp** (e.g. an unknown `drumClass`).

## Screenshots for docs

Use `scripts/capture-window.ps1`, the PrintWindow approach (never SetForegroundWindow or
SetCursorPos: Owen is often using the machine, and a mis-capture can grab his private
windows). Screenshots live in `assets/screenshots/`, referenced from README and docs. To reach
a control, do not synthesize clicks: posted WM_LBUTTONDOWN never reaches JUCE. Invoke the
button through UI Automation (`-InvokeButtons`, by accessible name); combo boxes are UIA
read-only, so `-SetValues "Melody=Drums"` opens the menu and invokes the row. Pass
`-WindowTitle` if the process owns more than one window. No screenshots are checked in yet.

## Rules

- Every interaction answers: can this be done with a click, a drag, or a scroll? Hit targets
  at least `okstudio::ui::minHitPx`.
- Use ripgrep (`rg`) for searching, never grep.
- Never add AI attribution to commits or published text.
- Keep docs current as work lands: CONTROLS for every control, DECISIONS for every round,
  CHANGELOG `[Unreleased]` for every user-visible change.

## Conventions

- Conventional commits (`feat:`, `fix:`, `docs:`, `chore:`).
- C++20, JUCE idioms, match surrounding style (`.clang-format`).
- Every text file is LF (`.gitattributes`).
- CI builds `windows-latest` only; don't pin a VS generator in workflows. Docs/assets-only
  pushes skip CI (`paths-ignore`).

## Sibling projects (same owner, same conventions)

`../okstudio-juce-kit` (shared code), `../Keys` (the kit's reference consumer: its
CMakeLists, tests, release tooling and docs set are the template this repo mirrors),
`../Undertow` (bass), `../Beatform` (drums), `../Contour`, `../Lattice`, `../alpha-osk`.
Installer, CI and signing mirror Keys; EV signing uses the same OK Studio eToken thumbprint.
Structural fixes made here are worth landing in the kit's `docs/USAGE.md` too.

**Murmur is the only plugin in the line with audio in.** Do not copy Keys's note-collector
`processBlock` or Contour's ppq scheduler wholesale: Murmur's processor feeds an engine with
audio and drains MIDI from it.
