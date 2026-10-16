# Changelog

All notable changes to Murmur are recorded here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/). Pre-1.0: parameter layout may still change
and break saved sessions (noted loudly when it does).

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
- Unit tests for processor state (`tests/StateTests.cpp`).

### Docs
- ARCHITECTURE, ABLETON_LIVE, BUILD, DESIGN.

### Kit
- `okstudio_add_plugin` gained an `AUDIO_EFFECT` option.
