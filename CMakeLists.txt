cmake_minimum_required(VERSION 3.22)

project(AICoreC
    VERSION 0.1.0
    DESCRIPTION "AICORE native C++ app (JUCE, zero browser)"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: read sources and emit execution charset as UTF-8 (PT-PT UI text).
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# ---------------------------------------------------------------------------
# Version bake (configure time) — Core core_version.h pattern.
# Writes generated/aicore_version.h with VERSION + git sha (short=10) and a
# .modified suffix when the tracked tree is dirty. A clean rebuild at HEAD
# therefore prints exactly the HEAD sha. See cmake/aicore_version.cmake.
# ---------------------------------------------------------------------------
set(AICORE_VERSION_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${AICORE_VERSION_GENERATED_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUT=${AICORE_VERSION_GENERATED_DIR}/aicore_version.h"
        "-DVERSION=${PROJECT_VERSION}"
        "-DALLOW_UNKNOWN=${AICORE_ALLOW_UNKNOWN_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/aicore_version.cmake"
    RESULT_VARIABLE _aicore_version_rc)
if(NOT _aicore_version_rc EQUAL 0)
    message(FATAL_ERROR "AICore-C: version bake failed (rc=${_aicore_version_rc})")
endif()
message(STATUS "AICore-C: version header -> ${AICORE_VERSION_GENERATED_DIR}/aicore_version.h")

# ---------------------------------------------------------------------------
# JUCE 9.0.5 (same pin as the Core DAW, but NOT taken from the Core: AICore and the
# Core are different programs, owner 2026-11-16). Prefer AICore's own copy, extracted
# from the pinned tarball (SHA256 below, checked 2026-11-16) into
# D:/AICore/development/deps/JUCE-9.0.5; otherwise FetchContent with URL + SHA256.
# ---------------------------------------------------------------------------
include(FetchContent)

set(AICORE_JUCE_LOCAL "D:/AICore/development/deps/JUCE-9.0.5" CACHE PATH "AICore's own JUCE 9.0.5 source")
set(_aicore_use_local_juce OFF)
if(EXISTS "${AICORE_JUCE_LOCAL}/CMakeLists.txt")
    file(STRINGS "${AICORE_JUCE_LOCAL}/CMakeLists.txt" _aicore_juce_proj
         REGEX "project\\(JUCE VERSION")
    if(_aicore_juce_proj MATCHES "project\\(JUCE VERSION 9\\.0\\.5")
        set(_aicore_use_local_juce ON)
    endif()
endif()

if(_aicore_use_local_juce AND NOT FETCHCONTENT_SOURCE_DIR_JUCE)
    set(FETCHCONTENT_SOURCE_DIR_JUCE "${AICORE_JUCE_LOCAL}"
        CACHE PATH "AICore's own JUCE 9.0.5 source" FORCE)
    message(STATUS "AICore-C: JUCE 9.0.5 from AICore's own copy: ${AICORE_JUCE_LOCAL}")
else()
    message(STATUS "AICore-C: JUCE 9.0.5 via FetchContent (pinned URL + SHA256)")
endif()

FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.5.tar.gz
    URL_HASH SHA256=d92207f51ba728d3d195b6d5e93ecd049717a915fc250ff6844c32ea7272315a)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# Modules: core (JUCE-free, CLI-testable) + app (JUCE GUI/CLI).
# ---------------------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(app)