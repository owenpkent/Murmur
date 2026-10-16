<#
  build.ps1 - configure + build Murmur (VST3 + Standalone), then copy the VST3 into
  Owen's Ableton custom folder so a rebuild is picked up on the next Live scan.

  Usage:
    .\build.ps1              # Release build, copy .vst3 to %USERPROFILE%\Ableton\vst3
    .\build.ps1 -NoCopy      # build only, don't touch the Ableton folder
    .\build.ps1 -Config Debug
    .\build.ps1 -Test        # also build + run the unit tests
    .\build.ps1 -Installer   # also build release\MurmurSetup-<version>.exe (needs Inno Setup 6)

  Needs JUCE at ..\JUCE and the kit at ..\okstudio-juce-kit (sibling checkouts).
#>
param(
    [string]$Config = "Release",
    [switch]$NoCopy,
    [switch]$Test,
    [switch]$Installer
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"

# Configure with the VS 2022 generator; disable the CMake-side copy (we copy below,
# so CI and -NoCopy behave identically).
cmake -B $build -S $root -G "Visual Studio 17 2022" -A x64 -DMURMUR_COPY_PLUGIN=OFF
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build $build --config $Config --target Murmur_VST3 Murmur_Standalone
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

if ($Test) {
    cmake --build $build --config $Config --target Murmur_tests
    if ($LASTEXITCODE -ne 0) { throw "Test build failed" }
    ctest --test-dir $build -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
}

$vst3 = Join-Path $build "Murmur_artefacts\$Config\VST3\Murmur.vst3"

if (-not $NoCopy) {
    $dest = Join-Path $env:USERPROFILE "Ableton\vst3"
    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Force -Path $dest | Out-Null }
    # A VST3 is a bundle (folder) on Windows; copy the whole thing.
    Copy-Item -Recurse -Force -Path $vst3 -Destination $dest
    Write-Host "Copied Murmur.vst3 -> $dest" -ForegroundColor Green
}

if ($Installer) {
    $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
    if (-not $iscc) { $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
    if (-not (Test-Path $iscc)) { throw "Inno Setup 6 (ISCC.exe) not found" }
    $cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
    if ($cmake -notmatch 'project\(Murmur VERSION ([0-9.]+)') { throw "Could not read version from CMakeLists.txt" }
    & $iscc "/DVersion=$($Matches[1])" (Join-Path $root "installer\murmur.iss")
    if ($LASTEXITCODE -ne 0) { throw "Installer build failed" }
}

Write-Host "Done ($Config). Artefacts under $build\Murmur_artefacts\$Config" -ForegroundColor Cyan
