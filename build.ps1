<#
  build.ps1 - configure + build Murmur and Murmur Host (VST3 + Standalone), sign, copy the VST3 into
  Owen's Ableton custom folder so a rebuild is picked up on the next Live scan, and
  optionally build the installer.

  Usage:
    .\build.ps1                # Release build, copy .vst3 to %USERPROFILE%\Ableton\vst3
    .\build.ps1 -NoCopy        # build only, don't touch the Ableton folder
    .\build.ps1 -Config Debug
    .\build.ps1 -Test          # also build + run the unit tests
    .\build.ps1 -Installer     # also build + SIGN release\MurmurSetup-<version>.exe
    .\build.ps1 -Installer -NoSign   # unsigned installer (layout and branding checks only)
    .\build.ps1 -Sign          # sign the built binaries without building an installer
    .\build.ps1 -PrintVersion  # print the version from CMakeLists.txt and exit

  Needs JUCE at ..\JUCE and the kit at ..\okstudio-juce-kit (sibling checkouts).
  Signing needs the OK Studio EV eToken, see docs/RELEASE.md. For a release, double-click
  release.py instead: it wraps this and verifies what came out.
#>
param(
    [string]$Config = "Release",
    [switch]$NoCopy,
    [switch]$Test,
    [switch]$Installer,
    [switch]$Sign,
    [switch]$NoSign,
    [switch]$PrintVersion
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$root = $PSScriptRoot

# project(Murmur VERSION ...) in CMakeLists.txt is the one place the version lives
# (docs/RELEASE.md "Version source of truth"). Read once here, reused below for the
# installer's /DVersion=, and exposed via -PrintVersion so release.py can ask this script
# for it instead of keeping a second copy of the regex.
$cmakeText = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmakeText -notmatch 'project\(Murmur VERSION ([0-9.]+)') { throw "Could not read version from CMakeLists.txt" }
$Version = $Matches[1]
if ($PrintVersion) {
    Write-Output $Version
    exit 0
}

# The auto-updater pins this exact thumbprint (OK Studio Inc. EV cert on the eToken), so a
# release signed with anything else is rejected by every client. One file, so release.py
# reads the same value rather than keeping its own copy.
$SignThumbprint = (Get-Content (Join-Path $root "installer\signing-thumbprint.txt") -Raw).Trim()
$TimestampUrl   = "http://timestamp.sectigo.com"
$doSign = ($Sign -or $Installer) -and -not $NoSign

function Get-SignTool {
    $candidates = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
        Sort-Object { [version]($_.Directory.Parent.Name) } -Descending
    if (-not $candidates) { throw "signtool.exe not found (install the Windows 10/11 SDK)" }
    $candidates[0].FullName
}

function Invoke-Sign {
    param([string[]]$Files)
    $signtool = Get-SignTool
    $cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $SignThumbprint -and $_.HasPrivateKey }
    if (-not $cert) {
        throw "OK Studio EV cert ($SignThumbprint) not available. Plug in the eToken (SafeNet client running), then rebuild. Use -NoSign for an unsigned dev build."
    }
    Write-Host "Signing $($Files.Count) file(s) with $($cert.Subject.Split(',')[0])..." -ForegroundColor Cyan
    & $signtool sign /sha1 $SignThumbprint /fd sha256 /tr $TimestampUrl /td sha256 /d "Murmur" @Files
    if ($LASTEXITCODE -ne 0) { throw "Signing failed (eToken PIN cancelled or token removed?)" }
    & $signtool verify /pa /v @Files | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Signature verification failed" }
}

# Configure with the VS 2022 generator; disable the CMake-side copy (we copy below, so CI and
# -NoCopy behave identically, and a locked destination warns instead of failing the build).
cmake -B build -S $root -G "Visual Studio 17 2022" -A x64 -DMURMUR_COPY_PLUGIN=OFF
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build build --config $Config --target Murmur_VST3 Murmur_Standalone MurmurHost_VST3 MurmurHost_Standalone
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

if ($Test) {
    cmake --build build --config $Config --target Murmur_tests
    if ($LASTEXITCODE -ne 0) { throw "Test build failed" }
    ctest --test-dir build -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
}

if ($doSign) {
    Invoke-Sign -Files @(
        (Join-Path $root "build\Murmur_artefacts\$Config\VST3\Murmur.vst3\Contents\x86_64-win\Murmur.vst3"),
        (Join-Path $root "build\Murmur_artefacts\$Config\Standalone\Murmur.exe"),
        (Join-Path $root "build\MurmurHost_artefacts\$Config\VST3\Murmur Host.vst3\Contents\x86_64-win\Murmur Host.vst3"),
        (Join-Path $root "build\MurmurHost_artefacts\$Config\Standalone\Murmur Host.exe"))
}

$copied = $true
if (-not $NoCopy) {
    $dest = Join-Path $env:USERPROFILE "Ableton\vst3"
    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Force -Path $dest | Out-Null }
    foreach ($bundle in @(@{ Art = "Murmur_artefacts"; Name = "Murmur.vst3" },
                          @{ Art = "MurmurHost_artefacts"; Name = "Murmur Host.vst3" })) {
        $src = Join-Path $root "build\$($bundle.Art)\$Config\VST3\$($bundle.Name)"
        try {
            # A VST3 is a bundle (folder) on Windows; copy the whole thing.
            Copy-Item -Recurse -Force -Path $src -Destination $dest -ErrorAction Stop
            Write-Host "Installed $($bundle.Name) -> $dest" -ForegroundColor Green
        } catch {
            $copied = $false
            Write-Host "WARNING: could not update $dest\$($bundle.Name)" -ForegroundColor Yellow
            Write-Host "  The DAW has it loaded (file locked). Unload it (or close the DAW), then rerun." -ForegroundColor Yellow
        }
    }
}

if ($Installer) {
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw "Inno Setup 6 not found. Install with: winget install JRSoftware.InnoSetup" }
    & $iscc "/DVersion=$Version" (Join-Path $root "installer\murmur.iss")
    if ($LASTEXITCODE -ne 0) { throw "Installer build failed" }

    $setupExe = Join-Path $root "release\MurmurSetup-$Version.exe"
    if ($doSign) {
        Invoke-Sign -Files @($setupExe)
        Write-Host "Installer signed: release\MurmurSetup-$Version.exe" -ForegroundColor Green
    } else {
        Write-Host "Installer: release\MurmurSetup-$Version.exe (UNSIGNED - clients will reject it!)" -ForegroundColor Yellow
    }
}

$state = if ($doSign) { "signed" } else { "unsigned" }
if ($copied) {
    Write-Host "Done ($Config, $state). Artefacts under $root\build\Murmur_artefacts\$Config" -ForegroundColor Cyan
} else {
    Write-Host "Done ($Config, $state), but at least one VST3 was NOT installed - unload it in the DAW and rerun." -ForegroundColor Yellow
}
