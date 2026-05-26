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
# ─────────────────────────────────────────────────────────────────────────────
add_executable(SaxFXTests
    test_placeholder.cpp
    test_ring_buffer.cpp
    test_sampler.cpp
    test_midi_mapper.cpp
    test_project.cpp
    test_onnx_inference.cpp
    test_inference_thread.cpp
    test_onnx_benchmark.cpp
    test_ai_classifier.cpp
    test_feature_extractor.cpp
    test_ai_mix_engine.cpp
    test_pingpongdelay.cpp
    test_ewi_filter.cpp
    test_slot_dynamics_ab.cpp
    test_scene_manager.cpp
    test_p0_load.cpp
    # DSP sources (pure C++, no JUCE)
    ../src/dsp/WsolaShifter.cpp
    ../src/dsp/Sampler.cpp
    ../src/dsp/PingPongDelay.cpp
    ../src/dsp/DspPipeline.cpp
    ../src/dsp/BpmDetector.cpp
    ../src/dsp/AiContentClassifier.cpp
    ../src/dsp/FeatureExtractor.cpp
    ../src/dsp/AiMixEngine.cpp
)

target_include_directories(SaxFXTests PRIVATE
    ../src        # allows #include "dsp/Foo.h"
    .             # allows #include "TestHelpers.h"
)

target_compile_definitions(SaxFXTests PRIVATE
    P0_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p0"
)

target_link_libraries(SaxFXTests PRIVATE
    Catch2::Catch2WithMain
)

# ONNX Runtime (optional, for AI inference tests)
if(SAXFX_ENABLE_ONNX)
    target_link_libraries(SaxFXTests PRIVATE OnnxRuntime::OnnxRuntime)
    target_compile_definitions(SaxFXTests PRIVATE
        SAXFX_HAS_ONNX=1
        IDENTITY_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/models/identity.onnx"
        CLASSIFIER_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/content_classifier.onnx"
        CLASSIFIER_NORM_PATH="${CMAKE_SOURCE_DIR}/models/content_classifier_norm.bin"
        DATASET_DIR="${CMAKE_SOURCE_DIR}/data/dataset"
        MIX_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/mix_model.onnx"
        MIX_MODEL_NORM_PATH="${CMAKE_SOURCE_DIR}/models/mix_model_norm.bin"
    )
    ort_copy_dll(SaxFXTests)
endif()

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
