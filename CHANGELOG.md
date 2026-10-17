# Changelog

All notable changes to Murmur are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/); versions are semver. Pre-1.0: parameter
layout may still change and break saved sessions (noted loudly when it does).

## [Unreleased]

### Added
- **Take Editor** (Live | Edit toggle): record a take, see every note as a blob on a pitch roll
  with your sung pitch curve through it, and drag blobs to fix the pitch (Move / Reset / Erase,
  Undo, Snap to scale, time zoom and Fit). Melody mode. **Play** runs the take's notes as MIDI
  through the instrument (or out of the track), from a start marker set by clicking the time
  ruler, with a playhead on the roll; an anchored take also follows the host transport. See `docs/CONTROLS.md` and `docs/TAKE_EDITOR_DESIGN.md`.
- **Drag MIDI** tile in the Take Editor: drag the take (with your pitch edits) out of the
  window into Live as a MIDI clip. The file is also kept in `<takes folder>/MIDI/`.
- **MCP take tools**: `get_take`, `edit_take` (shift, set, reset, delete, snap_to_scale, undo)
  and `take_transport` (record, stop_recording, play, stop). See `docs/MCP.md`.
- **Murmur Host** (`src/host/`, `MURMUR_BUILD_HOST`, second plugin target `MurmurHost`): an audio
  effect that hosts one instrument VST3 inside itself, plays the engine's MIDI through it and
  outputs the instrument, so voice in and drums or synth out work on one track. New **Mix**
  parameter (voice to instrument crossfade, default instrument only), Scan and instrument list,
  embedded instrument GUI, hosted plugin saved in state, latency = engine + instrument, and a
  missing instrument shows "Instrument not found" while audio keeps passing. The MIDI is still
  also sent out. `run.py` launches it by default (`--murmur` for plain Murmur); `build.ps1`
  builds and installs both VST3s; CI builds it. `tests/HostTests.cpp`. Design:
  `docs/MURMUR_HOST_DESIGN.md`. Not in the installer yet.
- Plugin shell on okstudio-juce-kit: VST3 audio effect (audio in, audio passed through,
  MIDI out), plus Standalone.
- Parameters: mode (Melody/Drums), sensitivity, quantize strength, swing, grid division,
  scale root, scale, scale lock.
- Mouse-only editor: big Melody | Drums toggle, three knobs, grid control, scale controls
  (Melody), Calibrate and Clear per drum class (Drums), live input level, last note or hit,
  free-tempo readout while the host is stopped.
- State save/load via the kit, including the drum calibration.
- Auto-updater pinned to `okstudio1/murmur-releases`.
- DSP engine: MPM pitch tracking, spectral-flux onsets, per-user kNN drum classification,
  dynamic quantize with free-tempo grid inference.
- **MCP server** (`src/mcp/MurmurMcp.*`, `murmur-mcp` stdio shim, `MURMUR_BUILD_MCP_SHIM`):
  Claude Code can read the live analysis and drive the controls. Tools: `get_state`,
  `get_transport`, `list_params`, `set_params`, `get_calibration`, `start_calibration`,
  `clear_calibration`. See `docs/MCP.md`.
- `MurmurProcessor::getHostBpm()` and two read-only engine getters
  (`getCalibratingClass`, `getNumCalibrationExamples`) for the MCP tools.
- Release tooling: `release.py` (build, sign, verify the installer), `build.ps1` gained signing
  (`-Sign`, `-NoSign`, installer signing) and `-PrintVersion`; generated icon and installer
  wizard images (`installer/generate_brand.py`, `assets/Murmur.svg`).
- Dev loop: `run.py` / `run.ps1` build and launch the Standalone.
- `scripts/capture-window.ps1` (docs screenshots) and `scripts/downloads.ps1` (release
  download counts).

### Changed
- The Standalone apps no longer start with the mic muted, and no longer play the voice out: you
  hear only the hosted instrument, so a mic and speakers cannot feed back. The VST3 is unchanged.
- CI builds Murmur Host and the MCP shim, cancels superseded runs, and checks out the private kit with a
  read-only deploy key (`KIT_DEPLOY_KEY`) instead of a token.
- `.gitattributes` forces LF in the repository.

### Tests
- `tests/DspTests.cpp` (engine), `tests/StateTests.cpp` (processor state and calibration round
  trip), `tests/McpTests.cpp` (MCP tools) and `tests/HostTests.cpp` (Murmur Host without an
  instrument), run by `ctest` as `Murmur_tests`.

### Docs
- ARCHITECTURE, ABLETON_LIVE, BUILD, DESIGN, plus CONTROLS (every control and its gesture),
  DECISIONS (dated log), REFERENCES (the research behind the engine), MCP, RELEASE and
  MURMUR_HOST_DESIGN.

### Kit
- `okstudio_add_plugin` gained an `AUDIO_EFFECT` option.
