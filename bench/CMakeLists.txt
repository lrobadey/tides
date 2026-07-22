cmake_minimum_required(VERSION 3.22)
project(TideBench VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Reuse the JUCE already fetched by the main build.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/juce-src
                 ${CMAKE_CURRENT_BINARY_DIR}/juce-build EXCLUDE_FROM_ALL)

juce_add_console_app(TideBench PRODUCT_NAME "TideBench")

target_sources(TideBench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../Source/GranulatorEngine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/bench.cpp)

target_include_directories(TideBench
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Source)

# Match the shipping plugin's optimisation posture: Release + recommended + LTO.
target_link_libraries(TideBench
    PRIVATE
        juce::juce_audio_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
