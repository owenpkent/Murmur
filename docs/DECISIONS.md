# Decisions

The dated log of how Murmur came to work the way it does: what was asked for, in Owen's own
words where they were the ask, what was built, and what each round superseded. CLAUDE.md's
Architecture section is the other half, the map of what is true today;
[DESIGN.md](DESIGN.md) is the standing rationale for the DSP choices and
[REFERENCES.md](REFERENCES.md) the sources behind them. Entries are newest first. A superseded
claim is kept where it sits and marked there rather than deleted, because knowing why a rule
existed is what stops it being reinvented.

**The MCP surface observes and configures; it does not play (2026-10-15).** The repo was
brought to Keys's structure, and Keys's MCP bridge was the template. Most of Keys's tools
(`play_notes`, `play_sequence`, the chord-pad and arp tools) have no Murmur equivalent, and
Murmur's analogue of "play a note" would be to fake a voice, which would test nothing about the
thing Murmur is for. What Claude can usefully do is the rest of what the editor does: set the
mode, key and quantize (`set_params`), read what the analysis is hearing (`get_state`,
`get_transport`), and run the drum calibration (`start_calibration`, `get_calibration`,
`clear_calibration`). `start_calibration` only arms the engine, exactly as the button does; the
three sounds have to come from a person at the mic.

- **No timer in the bridge.** Keys needs a 5 ms poll to emit scheduled notes; Murmur schedules
  nothing, so `MurmurMcp` is a plain object that owns the server and nothing else.
- **A bad `drumClass` is rejected, not clamped.** A typo that fell back to Kick would calibrate
  the wrong sound and report success. This is Keys's rule for out-of-range arguments.
- **The processor stores the host tempo.** `get_transport` has to say which clock the grid is
  on (host or inferred), and the engine keeps the host tempo private, so the processor now
  records it in an atomic (`getHostBpm()`), written once per block next to `hostPlaying`.
- **The engine gained two read-only getters** (`getCalibratingClass`,
  `getNumCalibrationExamples`) so the bridge can report calibration state under the classifier
  lock, instead of copying the whole calibration tree.
- **The shim is built with `PRODUCT murmur`**, so a client launching `murmur-mcp.exe` with no
  arguments finds Murmur and not another plugin of the line in the same DAW.

**The same round carried the rest of Keys's structure over (2026-10-15).** `release.py`,
`run.py`/`run.ps1`, `scripts/` (docs screenshots, download counts), `installer/generate_brand.py`
and the generated icon and wizard images, `build.ps1`'s `-PrintVersion`/signing, the CI deploy
key and concurrency group, and the docs set. Not carried over, because they are Keys content
rather than structure: the reference-manual and dataset folders, the chord, arpeggiator and
host code, and the docs that describe them. Murmur has no `LINE_INTERACTION.md` for the same
reason: in Keys it is about four arp lines hearing each other, and Murmur has one voice.

- **`run.py` launches Murmur Host (changed 2026-10-15).** It first launched the plain Standalone
  because Murmur had nothing to host. Murmur Host now exists, so, as in Keys, the default is the
  Host variant (a voice makes sound in-process); `--murmur` runs the plain one.
- **No `/STACK` raise in the tests.** Keys needs 8 MB because one test function holds eighty
  engine locals in one frame. Murmur's suite has no such frame; add the option the day it does.

**Audio effect, not instrument, not MIDI effect (2026-10-15).** Murmur takes audio in and emits
MIDI, which no other plugin in the line does, so the kit's `okstudio_add_plugin` gained an
`AUDIO_EFFECT` option for it. `acceptsMidi()` stays true because Live refuses a plugin with no
MIDI input bus (the same trap Keys documents); audio passes through unchanged. In Live it sits
on the vocal audio track and a MIDI track picks it as "MIDI From".

**The DSP engine and why each piece is what it is (2026-10-15).** Written in-house and
header-only under `src/dsp/`: MPM pitch tracking, spectral-flux (SuperFlux) onsets, per-user
kNN drum classification, strength/swing/grid quantize, and IOI-cluster free-tempo inference for
when the host is stopped. The reasons, and what was rejected (Basic Pitch, Cycfi Q, Gist,
aubio, BTrack, CREPE/ONNX, a fixed pretrained drum classifier), are in
[DESIGN.md](DESIGN.md); the papers are in [REFERENCES.md](REFERENCES.md). The processor is a
thin shell around `MurmurEngine`: mono mix in, `process`, `drainMidi` out.

- **Scale Lock defaults on.** Staying in key is the point of humming a melody.
- **Calibration is a ValueTree saved next to the APVTS**, wrapped in `MURMUR_CALIBRATION`, so a
  calibrated voice comes back with the session.
- **Drum notes are fixed General MIDI Drum Rack notes** (36, 38, 42, 46). There is no mapping
  UI; the engine exposes the table (`drumNotes()`) for one later.

**Murmur Host (2026-10-15).** Plain Murmur emits MIDI only, so Live needs a second track taking
"MIDI From Murmur". Murmur Host hosts one instrument VST3 so one audio track does it all.
Mirrors Keys Host (VST3 only, one slot, message-thread load bracketed by `suspendProcessing`,
state holds path plus plugin blob). It stays an `AUDIO_EFFECT` because it needs the voice in.
The Mix parameter exists only in the Host layout. Deliberately different from Keys Host: the
instrument list comes from an explicit Scan (instruments only, cached) rather than a publisher
folder browser; the instrument GUI is embedded below Murmur's UI rather than a floating window;
no fader bank. Not shipped in the installer, matching Keys, whose installer ships Keys only.
