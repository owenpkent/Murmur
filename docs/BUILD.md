# Building Murmur

## Prerequisites

- Windows 10/11, Visual Studio 2022 (or Build Tools), CMake 3.22+
- JUCE 8 at `..\JUCE`, the kit at `..\okstudio-juce-kit` (sibling checkouts)
- Python 3 for `run.py` and `release.py` (standard library only; `generate_brand.py` needs Pillow)
- Optional: Inno Setup 6 for the installer, the Windows SDK (`signtool`) and the EV eToken
  for signing

Override paths with `-DMURMUR_JUCE_PATH=<dir>` and `-DOKSTUDIO_KIT_PATH=<dir>`.

## Testing a change (run.py)

**Default dev loop: `run.py`.** Do not send Owen to Ableton to try a change. It closes the
running Standalone (politely, so JUCE saves the chosen audio device), builds exactly one
Standalone target (Murmur Host by default, `--murmur` for plain Murmur), and relaunches it. The Standalone opens on the machine's audio input, so
singing into the mic shows the level bar, the last note or hit and the free tempo with no DAW
involved and no rescan.

Owen runs it by double-clicking `run.py` in Explorer: no arguments, no terminal, and the
console holds open on failure so he can read the error. `run.ps1` is a thin shim over it, so
there is one copy of the logic.

```powershell
py run.py              # build + launch the Murmur Host Standalone
py run.py --murmur     # plain Murmur Standalone (MIDI only, silent)
py run.py --no-build   # just relaunch what is already built
py run.py --hold       # pause on failure (for a console that closes when this returns)
```

`--no-build` warns when a source file is newer than the binary, because a stale binary is
indistinguishable from a fresh one by looking at it.

Go to a real Live load test only for what the Standalone genuinely cannot show: bus layout,
plugin classification, the "MIDI From" routing, the installer, the updater, host automation.

### Smart App Control

Smart App Control is enforced on this machine and dev builds are unsigned, so the first launch
of a freshly linked exe can be held while Windows vets it. `run.py` waits that out with a
counter on screen (twenty minutes before it gives up; Ctrl+C stops waiting). It is the
reputation check, not the build: a no-op `--no-build` relaunch of an already-vetted exe is
immediate.

## Quick build (build.ps1)

```powershell
.\build.ps1                 # Release Murmur + Murmur Host (VST3 + Standalone), copy both VST3s to %USERPROFILE%\Ableton\vst3
.\build.ps1 -NoCopy         # build only
.\build.ps1 -Config Debug
.\build.ps1 -Test           # also build and run the tests
.\build.ps1 -Installer      # also build and SIGN release\MurmurSetup-<version>.exe
.\build.ps1 -Installer -NoSign   # unsigned, for checking layout and branding
.\build.ps1 -Sign           # sign the built binaries without making an installer
.\build.ps1 -PrintVersion   # print the version from CMakeLists.txt and exit
```

`%USERPROFILE%\Ableton\vst3` is Owen's Ableton custom VST3 folder; `%USERPROFILE%\VST3` is not
scanned. If the DAW has the plugin loaded the copy is locked: `build.ps1` warns instead of
failing, and the build is unchanged. Unload Murmur and rerun.

For a release, double-click `release.py` rather than running `-Installer` yourself: see
[RELEASE.md](RELEASE.md).

## Plain CMake

```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DMURMUR_COPY_PLUGIN=OFF
cmake --build build --config Release --target Murmur_VST3 Murmur_Standalone MurmurHost_VST3 MurmurHost_Standalone
cmake --build build --config Release --target murmur-mcp
cmake --build build --config Release --target Murmur_tests
ctest --test-dir build -C Release --output-on-failure
```

Artefacts land in `build/Murmur_artefacts/Release/{VST3,Standalone}/` (Murmur Host in
`build/MurmurHost_artefacts/...`); the MCP shim is
`build/Release/murmur-mcp.exe`. Passing `-DMURMUR_COPY_PLUGIN=OFF` builds but does **not**
install to the DAW folder, so Ableton keeps loading the previously installed binary. If Owen
reports "the VST didn't update", that is the usual cause: run `build.ps1`, then rescan in Live.

## Tests

`Murmur_tests` is a console JUCE UnitTest runner:

- `tests/DspTests.cpp`: the engine (pitch, onsets, drum classifier, quantize, inference). It
  owns `main`.
- `tests/StateTests.cpp`: processor state, bus configuration and the calibration round trip.
- `tests/McpTests.cpp`: the MCP tools, driven through the server's JSON-RPC layer.
- `tests/HostTests.cpp`: Murmur Host with no instrument loaded (Mix, passthrough, state, latency).
  Left out when `MURMUR_BUILD_HOST` is OFF.

The state, MCP and Host tests need a real processor (APVTS, state, the bridge), so the runner
links `MurmurHost` (a superset of Murmur), or `Murmur` when the Host is off. No `/STACK` raise is needed. The exit code is the failure count, so
`ctest` is the whole interface.

## CMake options

| Option | Default | Effect |
|--------|---------|--------|
| `MURMUR_COPY_PLUGIN` | ON | Copy the VST3 to `MURMUR_VST3_COPY_DIR` after building. `build.ps1` and CI turn it off. |
| `MURMUR_VST3_COPY_DIR` | `%USERPROFILE%/Ableton/vst3` | Where that copy goes. |
| `MURMUR_BUILD_HOST` | ON | Build Murmur Host (`MurmurHost_VST3`, `MurmurHost_Standalone`) and its tests. |
| `MURMUR_BUILD_MCP_SHIM` | ON | Build the `murmur-mcp` stdio bridge (see [MCP.md](MCP.md)). |
| `MURMUR_JUCE_PATH` | `../JUCE` | JUCE checkout. |
| `OKSTUDIO_KIT_PATH` | `../okstudio-juce-kit` | Kit checkout. |

## CI

`.github/workflows/ci.yml` builds both VST3s, the MCP shim and the tests on windows-latest, runs
the tests, and uploads the unsigned VST3s. The macOS leg was dropped across the line to save
Actions minutes. The kit is private: CI checks it out with a read-only deploy key stored as
the `KIT_DEPLOY_KEY` secret (no PAT). `security.yml` runs cppcheck. Dependabot bumps the
pinned actions weekly, batched into one PR.

## Installer

`installer/murmur.iss` (Inno Setup 6) installs `Murmur.vst3` into the VST3 folder (not Murmur Host). The output
name must be exactly `MurmurSetup-{version}.exe` for the auto-updater. Signing happens in
`build.ps1`; `installer/signing-thumbprint.txt` holds only the certificate thumbprint. The icon
and wizard images are generated from `assets/Murmur.svg` by `py installer\generate_brand.py`
and committed. Details: [RELEASE.md](RELEASE.md).

## Troubleshooting

- **"The VST didn't update" in Live.** See Plain CMake above: the copy was skipped or locked.
- **`signtool` returns `0x800704c7` at once.** The eToken PIN prompt went unanswered because
  the build was non-interactive. Run `release.py` from your own session.
- **Binary reports the previous version in its file properties.** Delete
  `build\*_artefacts\JuceLibraryCode\*_resources.rc` and rebuild (RELEASE.md step 1).
- **CI cannot check out the kit.** The `KIT_DEPLOY_KEY` secret is missing or the deploy key was
  revoked on the kit repo.
