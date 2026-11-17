# aicore_core — JUCE-free module (CLI-testable). No JUCE includes or linkage.
add_library(aicore_core STATIC
    src/version.cpp
    src/selftest.cpp)

target_include_directories(aicore_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${AICORE_VERSION_GENERATED_DIR}")

target_compile_features(aicore_core PUBLIC cxx_std_20)