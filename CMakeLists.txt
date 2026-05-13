cmake_minimum_required(VERSION 3.22)

project(SaxFXLive
    VERSION 0.1.0
    DESCRIPTION "Real-time audio processing for live saxophone"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Un seul dossier de sortie quelle que soit la config (Debug/Release/RelWithDebInfo)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin")

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
# ONNX Runtime (optionnel — inference IA, CPU pré-compilé)
# ─────────────────────────────────────────────────────────────────────────────
option(SAXFX_ENABLE_ONNX "Enable ONNX Runtime for AI inference" ON)

if(SAXFX_ENABLE_ONNX)
    include(cmake/FindOnnxRuntime.cmake)
    message(STATUS "ONNX Runtime ${ORT_VERSION} enabled")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# ASIO SDK (optionnel — WASAPI en fallback si absent)
# ─────────────────────────────────────────────────────────────────────────────
set(JUCE_ASIO_SDK_PATH "" CACHE PATH "Path to Steinberg ASIO SDK")

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
    ICON_BIG         "${CMAKE_CURRENT_SOURCE_DIR}/src/app_icon.png"
    ICON_SMALL       "${CMAKE_CURRENT_SOURCE_DIR}/src/app_icon.png"
)

# Forcer un seul dossier de sortie au niveau du target (JUCE override sinon)
set_target_properties(SaxFXLive PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin"
)

# Code source
target_sources(SaxFXLive PRIVATE
    src/Main.cpp
    src/MainComponent.cpp
    src/dsp/WsolaShifter.cpp
    src/dsp/Sampler.cpp
    src/dsp/DspPipeline.cpp
    src/dsp/BpmDetector.cpp
    src/dsp/PingPongDelay.cpp
    src/dsp/AiContentClassifier.cpp
    src/dsp/AiMixEngine.cpp
    src/dsp/FeatureExtractor.cpp
    src/dsp/SerumHost.cpp
    src/midi/MidiManager.cpp
    src/project/ProjectLoader.cpp
    src/ui/SaxFXLookAndFeel.cpp
    src/ui/SaxOsLookAndFeel.cpp
    src/ui/RotaryKnob.cpp
    src/ui/ScaleStaffComponent.cpp
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
    $<$<BOOL:${SAXFX_ENABLE_ONNX}>:SAXFX_HAS_ONNX=1>
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
)

# ASIO include path
if(JUCE_ASIO)
    target_include_directories(SaxFXLive PRIVATE "${JUCE_ASIO_SDK_PATH}")
endif()

# ONNX Runtime link
if(SAXFX_ENABLE_ONNX)
    target_link_libraries(SaxFXLive PRIVATE OnnxRuntime::OnnxRuntime)
    ort_copy_dll(SaxFXLive)
endif()

# Copy AI models next to the exe after each build
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models")
    add_custom_command(TARGET SaxFXLive POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/models"
            "$<TARGET_FILE_DIR:SaxFXLive>/models"
        COMMENT "Copying AI models to output directory"
    )
endif()

# Génère JuceHeader.h (inclut tous les modules JUCE liés)
juce_generate_juce_header(SaxFXLive)

# ── Binary resources (logo embarqué) ─────────────────────────────────────────
juce_add_binary_data(SaxFXAssets SOURCES src/logo.png)
target_link_libraries(SaxFXLive PRIVATE SaxFXAssets)

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
