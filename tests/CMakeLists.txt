cmake_minimum_required(VERSION 3.22)

# ─────────────────────────────────────────────────────────────────────────────
# Catch2 via FetchContent
# ─────────────────────────────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# ─────────────────────────────────────────────────────────────────────────────
# Cible de tests
# DSP sources compilées directement (pas de JUCE dependency dans src/dsp/)
# ─────────────────────────────────────────────────────────────────────────────
add_executable(SaxFXTests
    test_placeholder.cpp
    test_ring_buffer.cpp
    test_yin.cpp
    test_flanger.cpp
    test_harmonizer.cpp
    test_pipeline.cpp
    # DSP sources (pure C++, no JUCE)
    ../src/dsp/YinPitchTracker.cpp
    ../src/dsp/Flanger.cpp
    ../src/dsp/Harmonizer.cpp
    ../src/dsp/DspPipeline.cpp
)

target_include_directories(SaxFXTests PRIVATE
    ../src        # allows #include "dsp/Foo.h"
    .             # allows #include "TestHelpers.h"
)

target_link_libraries(SaxFXTests PRIVATE
    Catch2::Catch2WithMain
)

target_compile_features(SaxFXTests PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(SaxFXTests PRIVATE /W4 /WX-)
else()
    target_compile_options(SaxFXTests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Intégration CTest
include(CTest)
include(Catch)
catch_discover_tests(SaxFXTests)
