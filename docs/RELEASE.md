# Murmur Release & Publishing

Distribution mirrors Keys and the rest of the line, and so does this document: the installer
is built and EV-signed **locally**, then published as a GitHub Release on a separate public
repo. CI builds are unsigned smoke tests only, because the EV certificate lives on a hardware
token, so a release binary can only be produced on the dev machine with the eToken plugged in.

## The contract, and why every clause of it is load-bearing

Releases live in the separate public repo **`okstudio1/murmur-releases`**, and the in-plugin
updater is hard-pinned to it (`updaterConfig` in `src/PluginEditor.cpp`, which the kit's
`okstudio::updater` reads). The releases repo is separate from the source repo so the pinned URL never has to follow a
source rename or move.

Four hard contracts. **Every one of them fails silently**, which is the whole reason this file
exists rather than a shorter one:

- **Never move or rename the releases repo.** Every shipped copy of Murmur pins its API URL. A
  rename orphans every installed plugin, and they will report "up to date" for ever rather than
  erroring.
- **The asset must be named exactly `MurmurSetup-<version>.exe`.** The updater matches on
  `assetPrefix = "MurmurSetup-"` and ignores anything else in the release without complaint,
  so a release carrying a misnamed asset looks published and is invisible.
- **The tag must be `v<version>`**, matching the version in the asset name. The updater
  compares the tag against `MURMUR_VERSION`.
- **`gh release create` must always pass `--repo okstudio1/murmur-releases`.** Forget it and
  `gh` cheerfully creates the release on the *source* repo, where no updater will ever look.
  Tag the source repo for history; publish the binaries in the releases repo.

The updater's verification is **fail-closed** at every gate (see the kit's
`docs/AUTO_UPDATE.md`), so a release published unsigned is not merely unpolished: every client
rejects it outright.

**Status:** Murmur has not shipped a release yet. `okstudio1/murmur-releases` has to exist
(public) before the first one; create it before tagging `v0.1.0`.

## Version source of truth

`project(Murmur VERSION x.y.z)` in `CMakeLists.txt`. Nothing else hardcodes a version. Three
consumers read it, and all three derive rather than repeat:

| Consumer | How it gets there |
| --- | --- |
| The plugin's own reported version | `MURMUR_VERSION="${PROJECT_VERSION}"` compile definition, read by `updaterConfig.currentVersion` |
| The installer | `build.ps1` parses `project(Murmur VERSION ...)` out of `CMakeLists.txt` and passes `/DVersion=` to `installer/murmur.iss` |
| The asset name | `murmur.iss` sets `OutputBaseFilename=MurmurSetup-{#Version}` |

`.\build.ps1 -PrintVersion` prints it and exits before touching any build state; `release.py`
asks it rather than keeping a second copy of the regex. Bumping the one line in
`CMakeLists.txt` is the entire version bump.

## Release checklist

### 1. Bump the version

Edit `project(Murmur VERSION x.y.z)` in `CMakeLists.txt`. Semver.

**Then delete the stale version resources**, or the shipped binary reports the *previous*
version in its Windows file properties:

```powershell
Remove-Item build\*_artefacts\JuceLibraryCode\*_resources.rc
```

JUCE writes `<Target>_resources.rc` (the `VS_VERSION_INFO` block) at **build** time and then
never rewrites it, so a version bump alone leaves a file still carrying the last release's
number. Re-running `cmake -B build` does not fix it; only regenerating the file does. This is
the trap Keys hit cutting 0.2.0. Check it explicitly (`release.py` does too):

```powershell
(Get-Item "build\Murmur_artefacts\Release\VST3\Murmur.vst3\Contents\x86_64-win\Murmur.vst3").VersionInfo.FileVersion
```

### 2. Stamp the changelog

Move the `[Unreleased]` contents in `CHANGELOG.md` under a new `## [x.y.z] - YYYY-MM-DD`
heading and leave a fresh empty `[Unreleased]` above it. That section is also the release
notes, so read it once as a stranger would.

### 3. Commit

```powershell
git add CMakeLists.txt CHANGELOG.md
git commit -m "chore: release x.y.z"
```

### 4. Build and sign

Plug in the EV token. **From a non-elevated shell**: SafeNet exposes the certificate to the
user session only, so an elevated shell reports "OK Studio EV cert not available" even with
the token in.

**Double-click `release.py` in Explorer.** That is the whole step, and it has to be *your own*
session rather than an agent's: once the token's cached PIN expires the SafeNet driver puts up
a **GUI prompt**, a non-interactive build never shows it, and signtool returns
`ERROR_CANCELLED` (`0x800704c7`) at once, which reads exactly like a cancelled PIN and is not
one. `release.py` wraps the command below, then verifies what came out: fresh rather than left
over from an earlier build, signed, reporting the version `CMakeLists.txt` claims, and with
the branding files present.

```powershell
.\build.ps1 -Installer          # what release.py runs
py release.py --no-sign         # unsigned, to check layout and branding only
```

`-Installer` implies signing (`-NoSign` overrides for an unsigned test build). The pipeline is
a Release build of Murmur and Murmur Host (VST3 and Standalone), sign the VST3 and Standalone
binaries, copy the VST3s to the Ableton scan folder, compile the Inno Setup installer, then
sign and verify that. The installer packages `Murmur.vst3` only. Output: `release\MurmurSetup-x.y.z.exe` (`release/` is gitignored).
The token prompts for its PIN once, part way through.

Branding (`assets/Murmur.ico`, `installer/wizard_large.bmp`, `installer/wizard_small.bmp`) is
generated from `assets/Murmur.svg` by `py installer\generate_brand.py` and committed, so a
build never needs Pillow.

### 5. Validate before publishing

Two gates, and the second is not optional even when the first is green:

- **pluginval**: `pluginval --strictness-level 5 --validate <path to Murmur.vst3>`
  (https://github.com/Tracktion/pluginval).
- **A real Ableton Live load test.** Live enforces things pluginval does not. Murmur's
  particular risk is its bus layout and classification: it is an *audio effect* that outputs
  MIDI, `acceptsMidi()` must stay true (Live refuses a plug-in with no MIDI input bus), and
  Live's "MIDI From" routing is the only place that is really proven. Run the installer,
  confirm the VST3 lands where Live scans, rescan, put Murmur on a vocal audio track, route a
  MIDI track from it (see [ABLETON_LIVE.md](ABLETON_LIVE.md)), sing. Test an **upgrade over an
  existing version** too.

### 6. Tag and push

```powershell
git tag vX.Y.Z
git push origin main
git push origin vX.Y.Z
```

### 7. Publish, on the releases repo

```powershell
gh release create vX.Y.Z release\MurmurSetup-X.Y.Z.exe --repo okstudio1/murmur-releases --title "Murmur X.Y.Z" --notes-file <notes>
```

The `--repo` flag is mandatory. Release notes are that version's `CHANGELOG.md` section.

**Prepend the SmartScreen note to every release body.** Downloaders land on the release page
rather than the README, so the explanation for the install warning belongs where they are:

```markdown
> **Installing on Windows: about the SmartScreen prompt.**
> On first launch you may see a blue "Windows protected your PC" screen. This is normal for newer apps and does **not** mean anything is wrong. Murmur is digitally signed by **OK Studio Inc.** Click **More info** to see the publisher, then **Run anyway**.

---
```

A SmartScreen warning is reputation, not a signing failure. Do not go looking for a signing
bug when one appears.

### 8. End-to-end update test, from the second release onward

With the *previous* signed version installed, open Murmur and click the **Update to vX.Y.Z**
button in the header (it appears when the updater finds a newer release) through download,
signature verification and installer. Unsigned dev builds fail verification by design, so a
real published release is the only place the positive path is ever proven.

## Tracking downloads

```powershell
.\scripts\downloads.ps1
```

Wraps `gh api repos/okstudio1/murmur-releases/releases --paginate` and sums each release's
asset download counts. The count includes auto-updater fetches as well as manual clicks, and
GitHub does not distinguish them, so treat it as directional: downloads, not unique installs.

## Prerequisites (one-time)

- **Inno Setup 6**: `winget install JRSoftware.InnoSetup`
- **Windows SDK** for `signtool.exe`; `build.ps1` finds the newest one itself
- **SafeNet drivers + the EV token**, the same certificate the rest of the line uses
  (thumbprint pinned in `installer/signing-thumbprint.txt`, the one copy `build.ps1` and
  `release.py` both read, and pinned again by every client's updater)
- **`gh`**, authenticated against an account with write access to `okstudio1/murmur-releases`

## CI

`.github/workflows/ci.yml` builds on `windows-latest` for every push and PR (both VST3s, the
`murmur-mcp` shim and the tests), runs the tests, and uploads the unsigned VST3 as an
artifact. It cannot sign and it cannot publish; it is a smoke test. Docs and assets-only
pushes skip it via `paths-ignore`. The private kit is checked out with a read-only deploy key
stored as the `KIT_DEPLOY_KEY` secret (no PAT). `security.yml` runs cppcheck.

## Future work

- **SBOM.** Murmur's dependency surface is exactly JUCE plus `okstudio-juce-kit`, both pinned,
  so the honest equivalent of an SBOM is a small build manifest recording those two commits.
  Worth adding when distribution needs a build record, rather than before.
- **macOS**: VST3 + AU from the same CMakeLists, then codesign and notarize with an Apple
  Developer certificate. The Windows EV token has nothing to do with it.
