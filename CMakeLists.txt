cmake_minimum_required(VERSION 3.22)

project(TideGrains VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.13
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(JUCE)

juce_add_plugin(TideGrains
    COMPANY_NAME "Luca Robadey"
    BUNDLE_ID "com.lucarobadey.tides"
    PLUGIN_MANUFACTURER_CODE Lrbd
    PLUGIN_CODE TdGr
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "TIDES"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "TIDES needs microphone access to process live audio input."
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_COPY_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3"
)

target_sources(TideGrains
    PRIVATE
        Source/GranulatorEngine.cpp
        Source/GranulatorEngine.h
        Source/GrainTelemetry.h
        Source/ParameterIDs.h
        Source/OutputSafetyLimiter.cpp
        Source/OutputSafetyLimiter.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/WaveformHistory.cpp
        Source/WaveformHistory.h
)

target_compile_definitions(TideGrains
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(TideGrains
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

enable_testing()

juce_add_console_app(TideGrainsTests
    PRODUCT_NAME "TIDES Tests"
)

target_sources(TideGrainsTests
    PRIVATE
        Source/GranulatorEngine.cpp
        Source/GranulatorEngine.h
        Source/GrainTelemetry.h
        Source/ParameterIDs.h
        Source/OutputSafetyLimiter.cpp
        Source/OutputSafetyLimiter.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/WaveformHistory.cpp
        Source/WaveformHistory.h
        Tests/GranulatorEngineTests.cpp
)

target_compile_definitions(TideGrainsTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(TideGrainsTests
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

add_test(NAME TideGrainsTests COMMAND TideGrainsTests)
