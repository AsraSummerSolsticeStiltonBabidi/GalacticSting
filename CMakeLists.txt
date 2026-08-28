cmake_minimum_required(VERSION 3.22)

project(FractalAdditive VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile with UTF-8 so a non-ASCII product name (Fibló Galàctic) survives into the
# binary correctly instead of being re-encoded to the system codepage (mojibake).
if(MSVC)
    add_compile_options(/utf-8)
endif()

# JUCE does not use C++20 modules; disabling the scanner avoids a Ninja/MSVC
# rule-generation bug and speeds configuration up.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# On Windows, prefer static linking of the MSVC runtime so the standalone /
# VST3 run without the user needing the VC++ redistributable installed.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

#-------------------------------------------------------------------------------
# JUCE is downloaded automatically (pinned to a stable release).
#-------------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.14
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

#-------------------------------------------------------------------------------
# The plugin target (Standalone app + VST3).
#-------------------------------------------------------------------------------
juce_add_plugin(FractalAdditive
    COMPANY_NAME                "Asra3"
    BUNDLE_ID                   com.asrabeats.fractaladditive
    PLUGIN_MANUFACTURER_CODE    Asrb
    PLUGIN_CODE                 Frac
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE          # timbre->rhythm MIDI out (RhythmEngine)
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE          # required for computer-keyboard play
    VST3_CATEGORIES             "Instrument" "Synth"
    FORMATS                     Standalone VST3
    # ASCII on purpose. PRODUCT_NAME is what names the .vst3 file, the bundle folder and
    # the standalone .exe, and a non-ASCII filename broke three separate things: the
    # linker warned it may not load at all under a codepage other than 1252 (LNK4232),
    # JUCE's moduleinfo.json helper wrote the accents through the console codepage so
    # Ableton read mojibake, and the mangled PATH stopped any post-build repair from even
    # finding the file. The DISPLAYED name keeps its accents - see JucePlugin_Name below.
    PRODUCT_NAME                "Fiblo Galactic")

# Embedded assets (title font + the one-shot startup jingle).
juce_add_binary_data(GalacticAssets SOURCES assets/fonts/Kablammo.ttf assets/audio/jingle.wav)

target_sources(FractalAdditive PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/FloatingWindowOwner.cpp
    source/dsp/Spectrum.cpp
    third_party/MTS-ESP/libMTSClient.cpp)

target_include_directories(FractalAdditive PRIVATE source third_party/MTS-ESP)

target_compile_definitions(FractalAdditive PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(FractalAdditive
    PRIVATE
        GalacticAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# (No moduleinfo.json repair step: with an ASCII PRODUCT_NAME, JUCE's helper has no
# accents to mangle in the first place. The displayed name keeps its accents because
# AudioProcessor::getName() returns them directly - see PluginProcessor.h.)

#-------------------------------------------------------------------------------
# The search that CHOSE the palette (JUCE-free, like the palette itself). Committed
# because the colours in GalacticPalette.h are a RESULT: without the derivation in the
# tree they are eleven magic hexes nobody can re-derive or extend.
#-------------------------------------------------------------------------------
add_executable(PaletteSearch tools/PaletteSearch.cpp)
target_include_directories(PaletteSearch PRIVATE source)
set_target_properties(PaletteSearch PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

#-------------------------------------------------------------------------------
# Unit tests for the (JUCE-free) spectrum compiler.
#-------------------------------------------------------------------------------
add_executable(SpectrumTests
    tests/SpectrumTests.cpp
    source/dsp/Spectrum.cpp)
target_include_directories(SpectrumTests PRIVATE source)
set_target_properties(SpectrumTests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

#-------------------------------------------------------------------------------
# Unit tests for the microtuning subsystem (needs juce_core + the MTS client).
#-------------------------------------------------------------------------------
add_executable(TuningTests
    tests/TuningTests.cpp
    third_party/MTS-ESP/libMTSClient.cpp)
target_include_directories(TuningTests PRIVATE source third_party/MTS-ESP)
target_link_libraries(TuningTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags)
set_target_properties(TuningTests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

#-------------------------------------------------------------------------------
# Offline FFT verification of the rendered synth output (instantiates the real
# processor, renders audio, FFTs it, checks every partial is present).
#-------------------------------------------------------------------------------
juce_add_console_app(RhythmVerify PRODUCT_NAME "RhythmVerify")
target_sources(RhythmVerify PRIVATE
    tools/RhythmVerify.cpp
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/FloatingWindowOwner.cpp
    source/dsp/Spectrum.cpp
    third_party/MTS-ESP/libMTSClient.cpp)
target_include_directories(RhythmVerify PRIVATE source third_party/MTS-ESP)
target_compile_definitions(RhythmVerify PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)
target_link_libraries(RhythmVerify PRIVATE
    GalacticAssets
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_audio_processors
    juce::juce_recommended_config_flags)

juce_add_console_app(FftVerify PRODUCT_NAME "FftVerify")
target_sources(FftVerify PRIVATE
    tools/FftVerify.cpp
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/FloatingWindowOwner.cpp
    source/dsp/Spectrum.cpp
    third_party/MTS-ESP/libMTSClient.cpp)
target_include_directories(FftVerify PRIVATE source third_party/MTS-ESP)
target_compile_definitions(FftVerify PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)
target_link_libraries(FftVerify PRIVATE
    GalacticAssets
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_audio_processors
    juce::juce_recommended_config_flags)
