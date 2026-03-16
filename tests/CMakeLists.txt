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
    # Sprint 2+ : ajouter ici les fichiers de tests DSP
    # test_pitch_tracker.cpp
    # test_reverb.cpp
    # test_harmonizer.cpp
)

target_link_libraries(SaxFXTests PRIVATE
    Catch2::Catch2WithMain
    # Sprint 2+ : lier les modules DSP ici
)

target_compile_features(SaxFXTests PRIVATE cxx_std_17)

# Intégration CTest
include(CTest)
include(Catch)
catch_discover_tests(SaxFXTests)
