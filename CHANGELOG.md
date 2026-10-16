# Changelog

All notable changes to Murmur are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/); versions are semver. Pre-1.0: parameter
layout may still change and break saved sessions (noted loudly when it does).

## [Unreleased]

### Added
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
- CI builds the MCP shim, cancels superseded runs, and checks out the private kit with a
  read-only deploy key (`KIT_DEPLOY_KEY`) instead of a token.
- `.gitattributes` forces LF in the repository.

### Tests
- Unit tests for processor state (`tests/StateTests.cpp`) and the MCP tools
  (`tests/McpTests.cpp`).

### Docs
- ARCHITECTURE, ABLETON_LIVE, BUILD, DESIGN, plus CONTROLS (every control and its gesture),
  DECISIONS (dated log), REFERENCES (the research behind the engine), MCP and RELEASE.

### Kit
- `okstudio_add_plugin` gained an `AUDIO_EFFECT` option.
