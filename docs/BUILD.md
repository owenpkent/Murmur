# Build

## Needs

- Windows 10/11, Visual Studio 2022 (or Build Tools), CMake 3.22+
- JUCE 8 at `..\JUCE`, the kit at `..\okstudio-juce-kit` (sibling checkouts)
- Optional: Inno Setup 6 for the installer

Override paths with `-DMURMUR_JUCE_PATH=<dir>` and `-DOKSTUDIO_KIT_PATH=<dir>`.

## Commands

```powershell
.\build.ps1                 # Release VST3 + Standalone, copy VST3 to %USERPROFILE%\Ableton\vst3
.\build.ps1 -NoCopy         # build only
.\build.ps1 -Config Debug
.\build.ps1 -Test           # also build and run tests
.\build.ps1 -Installer      # also build release\MurmurSetup-<version>.exe
```

Manually:

```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DMURMUR_COPY_PLUGIN=OFF
cmake --build build --config Release --target Murmur_VST3 Murmur_Standalone
cmake --build build --config Release --target Murmur_tests
ctest --test-dir build -C Release --output-on-failure
```

Artefacts land in `build/Murmur_artefacts/Release/{VST3,Standalone}/`.

## Tests

`Murmur_tests` is a console JUCE UnitTest runner built from `tests/DspTests.cpp` (engine,
owns `main`) and `tests/StateTests.cpp` (processor state). It links the Murmur plugin target
because the state tests need a real processor.

## CI

`.github/workflows/ci.yml` builds the VST3 and the tests on windows-latest and uploads the
VST3 (the macOS leg was dropped across the line to save Actions minutes). The kit is private:
add a `KIT_REPO_TOKEN` secret that can read it. `security.yml` runs cppcheck.

## Installer

`installer/murmur.iss` (Inno Setup 6) installs `Murmur.vst3` into the VST3 folder. The
output name must be exactly `MurmurSetup-{version}.exe` for the auto-updater. Signing is done
outside the repo; `installer/signing-thumbprint.txt` holds only the certificate thumbprint.
