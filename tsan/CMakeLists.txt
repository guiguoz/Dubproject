cmake_minimum_required(VERSION 3.22)
project(EngineTSan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── ThreadSanitizer (Clang/GCC uniquement) ──────────────────────────────────
if(NOT MSVC)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
endif()

# ── Catch2 via FetchContent ──────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# ── signalsmith-stretch ──────────────────────────────────────────────────────
# signalsmith-stretch est déjà dans third_party/ et a FetchContent pour linear
add_subdirectory(${CMAKE_SOURCE_DIR}/../third_party/signalsmith-stretch
                 ${CMAKE_BINARY_DIR}/signalsmith-stretch-build)

# ── EngineTests (sans JUCE, sans ONNX) ──────────────────────────────────────
add_executable(EngineTSan
    # Tests
    ${CMAKE_SOURCE_DIR}/../tests/engine/test_tsan.cpp
    # Sources moteur (hors EngineFacade.cpp qui dépend de JUCE)
    ${CMAKE_SOURCE_DIR}/../src/engine/SlotPlayer.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/StretchConform.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/Analysis/BpmDetector.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/Analysis/AiContentClassifier.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/ImportPipeline.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/TransitionEngine.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/AutoMixDub.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/AudioGraph.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/OfflineRender.cpp
    ${CMAKE_SOURCE_DIR}/../src/engine/fx/PingPongDelay.cpp
)

target_include_directories(EngineTSan PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/engine/Analysis
    ${CMAKE_SOURCE_DIR}/../third_party/signalsmith-stretch
)

target_link_libraries(EngineTSan PRIVATE
    Catch2::Catch2WithMain
    signalsmith-stretch
)

if(MSVC)
    target_compile_options(EngineTSan PRIVATE /W4 /WX-)
else()
    # signalsmith-linear/fft.h utilise std::memcpy sans inclure <cstring>
    target_compile_options(EngineTSan PRIVATE -Wall -Wextra -Wno-unused-parameter -include cstring)
endif()
