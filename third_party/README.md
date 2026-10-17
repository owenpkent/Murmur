# Third-party code

Vendored header-only libraries. Each is copied from a clean upstream checkout of the pinned
tag, with no local modifications (files are byte-identical to upstream). Only what compiling
needs is kept, plus the licence and README. The `include/` folders are the upstream forwarding
headers (they are symlinks upstream; here they are the plain text files that Git on Windows
makes of them, which work the same). The optional `platform/` backends of signalsmith-linear
(Accelerate, IPP, PFFFT, CMSIS-DSP, XSimd) are not vendored: Murmur uses linear's built-in FFT.

| Library | Version | Commit | Licence | Upstream | Vendored |
|---|---|---|---|---|---|
| signalsmith-stretch | 1.4.0 | a670068 | MIT | https://github.com/Signalsmith-Audio/signalsmith-stretch | 2026-10-17 |
| signalsmith-linear | 0.6.4 | de55e6a | MIT | https://github.com/Signalsmith-Audio/linear | 2026-10-17 |

No local modifications.

signalsmith-stretch is the pitch shifter for the Take Editor voice render
(`src/take/VoiceRenderer.h`, docs/TAKE_EDITOR_DESIGN.md, Phase B). signalsmith-linear is its
one dependency (FFT and STFT).

To update, re-copy from a clean checkout of the new tag and change this table.
