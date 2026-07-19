cmake_minimum_required(VERSION 3.22)

project(FractalAdditive VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    COMPANY_NAME                "ASRA Beats"
    BUNDLE_ID                   com.asrabeats.fractaladditive
    PLUGIN_MANUFACTURER_CODE    Asrb
    PLUGIN_CODE                 Frac
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE          # required for computer-keyboard play
    VST3_CATEGORIES             "Instrument" "Synth"
    FORMATS                     Standalone VST3
    PRODUCT_NAME                "Fractal Additive")

target_sources(FractalAdditive PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
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
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

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
juce_add_console_app(FftVerify PRODUCT_NAME "FftVerify")
target_sources(FftVerify PRIVATE
    tools/FftVerify.cpp
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/dsp/Spectrum.cpp
    third_party/MTS-ESP/libMTSClient.cpp)
target_include_directories(FftVerify PRIVATE source third_party/MTS-ESP)
target_compile_definitions(FftVerify PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)
target_link_libraries(FftVerify PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_audio_processors
    juce::juce_recommended_config_flags)
