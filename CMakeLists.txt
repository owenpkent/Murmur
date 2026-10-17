cmake_minimum_required(VERSION 3.22)

project(Murmur VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: sibling checkout by default (../JUCE); override with -DMURMUR_JUCE_PATH=<path>.
set(MURMUR_JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "Path to a JUCE checkout")
add_subdirectory("${MURMUR_JUCE_PATH}" "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)

# okstudio-juce-kit: shared code for the line. Added AFTER JUCE (it links JUCE modules).
set(OKSTUDIO_KIT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../okstudio-juce-kit" CACHE PATH "Path to okstudio-juce-kit")
add_subdirectory("${OKSTUDIO_KIT_PATH}" "${CMAKE_BINARY_DIR}/okstudio-juce-kit")
include("${OKSTUDIO_KIT_CMAKE_DIR}/OKStudioPlugin.cmake")

# Where dev builds drop the VST3 so the DAW picks it up (Ableton custom folder).
# build.ps1 overrides / disables this; CI turns it off.
if (WIN32)
    set(MURMUR_VST3_COPY_DIR "$ENV{USERPROFILE}/Ableton/vst3" CACHE PATH "Folder the VST3 is copied to after build")
endif()
option(MURMUR_COPY_PLUGIN "Copy the VST3 to MURMUR_VST3_COPY_DIR after building" ON)

set(_murmurCopy "")
if (MURMUR_COPY_PLUGIN AND WIN32)
    set(_murmurCopy COPY_DIR "${MURMUR_VST3_COPY_DIR}")
endif()

# Every Murmur plugin target shares one source list and one set of definitions, so Murmur and
# Murmur Host cannot drift apart (Keys does the same with keys_configure_target).
function(murmur_configure_target tgt)
    target_sources(${tgt} PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
        src/StandaloneSupport.cpp
        src/ui/TakeView.cpp
        src/mcp/MurmurMcp.cpp)

    target_compile_definitions(${tgt} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        MURMUR_VERSION="${PROJECT_VERSION}")

    if (WIN32)
        target_link_libraries(${tgt} PRIVATE wintrust crypt32)
    endif()

    target_link_libraries(${tgt}
        PRIVATE
            okstudio_kit
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_data_structures
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endfunction()

# Murmur takes AUDIO IN (a voice) and emits MIDI, so it is an audio effect, not an
# instrument and not a MIDI effect. See the kit's okstudio_add_plugin AUDIO_EFFECT.
okstudio_add_plugin(Murmur
    PRODUCT_NAME  "Murmur"
    PLUGIN_CODE   Mrmr
    BUNDLE_SUFFIX murmur
    AUDIO_EFFECT
    ${_murmurCopy})
murmur_configure_target(Murmur)

# Murmur Host: Murmur plus one hosted instrument VST3 in the same plugin, so voice in and
# drums or synth out work on ONE audio track (plain Murmur needs a second MIDI track taking
# "MIDI From Murmur"). Still an AUDIO_EFFECT, because it needs the voice audio in.
# Design: docs/MURMUR_HOST_DESIGN.md.
option(MURMUR_BUILD_HOST "Build Murmur Host (hosts an instrument VST3 inside Murmur)" ON)
if (MURMUR_BUILD_HOST)
    okstudio_add_plugin(MurmurHost
        PRODUCT_NAME  "Murmur Host"
        PLUGIN_CODE   MrHo
        BUNDLE_SUFFIX murmurhost
        AUDIO_EFFECT
        ${_murmurCopy})
    murmur_configure_target(MurmurHost)
    target_sources(MurmurHost PRIVATE
        src/host/MurmurHostProcessor.cpp
        src/host/MurmurHostEditor.cpp)
    # MURMUR_HOST gates createPluginFilter (see PluginProcessor.cpp);
    # JUCE_PLUGINHOST_VST3 turns on JUCE's bundled VST3 hosting.
    target_compile_definitions(MurmurHost PUBLIC
        MURMUR_HOST=1
        JUCE_PLUGINHOST_VST3=1)
endif()

# The murmur-mcp stdio bridge: a small process Claude Code (or any local MCP client)
# launches that discovers the live plugin instance's loopback server and speaks MCP stdio
# to it. The default --product filter is baked in so a client launching it with no
# arguments finds Murmur and not another plugin of the line (see docs/MCP.md).
option(MURMUR_BUILD_MCP_SHIM "Build the murmur-mcp stdio bridge exe" ON)
if (MURMUR_BUILD_MCP_SHIM)
    okstudio_add_mcp_shim(murmur-mcp PRODUCT murmur)
endif()

# Unit tests (console, JUCE UnitTest). Build + run:
#   cmake --build build --target Murmur_tests --config Release && ctest --test-dir build -C Release
# DspTests.cpp owns main(); the other files are self-registering statics only.
# They need real processors (APVTS, state, the MCP bridge), so the tests link a plugin target.
# That is MurmurHost when it is built (a strict superset of Murmur: same sources, plus the
# host classes HostTests.cpp exercises; the only MURMUR_HOST-gated code is createPluginFilter,
# which no test calls), otherwise plain Murmur and HostTests.cpp is left out.
enable_testing()
set(_murmurTestSources tests/DspTests.cpp tests/StateTests.cpp tests/McpTests.cpp tests/TakeTests.cpp tests/TakeCaptureTests.cpp tests/TakePlaybackTests.cpp tests/TakeMidiTests.cpp)
if (MURMUR_BUILD_HOST)
    set(_murmurTestTarget MurmurHost)
    list(APPEND _murmurTestSources tests/HostTests.cpp)
else()
    set(_murmurTestTarget Murmur)
endif()
add_executable(Murmur_tests ${_murmurTestSources})
target_include_directories(Murmur_tests PRIVATE src)
target_link_libraries(Murmur_tests PRIVATE okstudio_kit juce::juce_dsp juce::juce_data_structures ${_murmurTestTarget})
add_test(NAME Murmur_tests COMMAND Murmur_tests)
