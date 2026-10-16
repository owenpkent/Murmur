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

# Murmur takes AUDIO IN (a voice) and emits MIDI, so it is an audio effect, not an
# instrument and not a MIDI effect. See the kit's okstudio_add_plugin AUDIO_EFFECT.
okstudio_add_plugin(Murmur
    PRODUCT_NAME  "Murmur"
    PLUGIN_CODE   Mrmr
    BUNDLE_SUFFIX murmur
    AUDIO_EFFECT
    ${_murmurCopy})

target_sources(Murmur PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)

target_compile_definitions(Murmur PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    MURMUR_VERSION="${PROJECT_VERSION}")

if (WIN32)
    target_link_libraries(Murmur PRIVATE wintrust crypt32)
endif()

target_link_libraries(Murmur
    PRIVATE
        okstudio_kit
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_data_structures
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Unit tests (console, JUCE UnitTest). Build + run:
#   cmake --build build --target Murmur_tests --config Release && ctest --test-dir build -C Release
# DspTests.cpp owns main(); StateTests.cpp is self-registering statics only. StateTests
# needs a real processor (APVTS + state), so the tests link the Murmur plugin target.
enable_testing()
add_executable(Murmur_tests tests/DspTests.cpp tests/StateTests.cpp)
target_include_directories(Murmur_tests PRIVATE src)
target_link_libraries(Murmur_tests PRIVATE okstudio_kit juce::juce_dsp juce::juce_data_structures Murmur)
add_test(NAME Murmur_tests COMMAND Murmur_tests)
