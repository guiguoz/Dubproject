cmake_minimum_required(VERSION 3.22)

project(SaxFXLive
    VERSION 0.1.0
    DESCRIPTION "Real-time audio processing for live saxophone"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────────────────────────
# JUCE — via submodule dans third_party/JUCE
# ─────────────────────────────────────────────────────────────────────────────
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE submodule not found.\n"
        "Run: git submodule update --init --recursive"
    )
endif()

add_subdirectory(third_party/JUCE)

# ─────────────────────────────────────────────────────────────────────────────
# ASIO SDK (optionnel — WASAPI en fallback si absent)
# ─────────────────────────────────────────────────────────────────────────────
option(JUCE_ASIO_SDK_PATH "" "Path to Steinberg ASIO SDK")

if(JUCE_ASIO_SDK_PATH AND EXISTS "${JUCE_ASIO_SDK_PATH}/common/asio.h")
    message(STATUS "ASIO SDK found at: ${JUCE_ASIO_SDK_PATH}")
    set(JUCE_ASIO ON)
else()
    message(STATUS "ASIO SDK not found — using WASAPI fallback")
    set(JUCE_ASIO OFF)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Application principale
# ─────────────────────────────────────────────────────────────────────────────
juce_add_gui_app(SaxFXLive
    PRODUCT_NAME     "SaxFX Live"
    VERSION          "0.1.0"
    COMPANY_NAME     "Guillaume"
    BUNDLE_ID        "com.guillaume.saxfxlive"
    NEEDS_MIDI_INPUT  TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT    FALSE
)

# Code source
target_sources(SaxFXLive PRIVATE
    src/Main.cpp
    src/MainComponent.cpp
    src/dsp/YinPitchTracker.cpp
    src/dsp/Flanger.cpp
    src/dsp/Harmonizer.cpp
    src/dsp/Sampler.cpp
    src/dsp/DspPipeline.cpp
    src/midi/MidiManager.cpp
    src/project/ProjectLoader.cpp
)

target_include_directories(SaxFXLive PRIVATE src)

# Modules JUCE requis
target_link_libraries(SaxFXLive PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# Définitions JUCE
target_compile_definitions(SaxFXLive PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SaxFXLive,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SaxFXLive,JUCE_VERSION>"
    $<$<BOOL:${JUCE_ASIO}>:JUCE_ASIO=1>
)

# ASIO include path
if(JUCE_ASIO)
    target_include_directories(SaxFXLive PRIVATE "${JUCE_ASIO_SDK_PATH}")
endif()

# Génère JuceHeader.h (inclut tous les modules JUCE liés)
juce_generate_juce_header(SaxFXLive)

# Warnings
if(MSVC)
    target_compile_options(SaxFXLive PRIVATE /W4 /WX-)
else()
    target_compile_options(SaxFXLive PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
