cmake_minimum_required(VERSION 3.22)
project(whisper_destilado LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -----------------------------------------------------------------------------
# third_party/whisper.cpp — force options BEFORE add_subdirectory
# (whisper.cpp defaults BUILD_SHARED_LIBS=ON on MSVC, which would produce DLLs)
# -----------------------------------------------------------------------------
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER    OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN             ON  CACHE BOOL "" FORCE)
set(GGML_AVX2               ON  CACHE BOOL "" FORCE)

if (WIN32)
    # MAX_PATH workaround: vulkan-shaders-gen sub-build creates long paths;
    # keep whisper.cpp's binary dir short, under the user profile.
    if (DEFINED ENV{USERPROFILE})
        set(_wd_short_base "$ENV{USERPROFILE}/wd_d")
    else()
        set(_wd_short_base "${CMAKE_BINARY_DIR}/d")
    endif()
    add_subdirectory(third_party/whisper.cpp "${_wd_short_base}/w-build")
else()
    add_subdirectory(third_party/whisper.cpp)
endif()

# -----------------------------------------------------------------------------
# third_party/soxr
# -----------------------------------------------------------------------------
# soxr 0.1.3 declares cmake_minimum_required(2.8.x); CMake >= 4 refuses it
# unless given a policy floor. third_party is read-only, so set it here.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
# soxr resolves its cmake modules via CMAKE_SOURCE_DIR (assumes top-level);
# as a subproject that points here, so hand it the module path ourselves.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/soxr/cmake/Modules)
set(BUILD_TESTS    OFF CACHE BOOL "Build soxr tests" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build soxr examples" FORCE)
set(WITH_OPENMP    OFF CACHE BOOL "Build soxr with OpenMP" FORCE)
add_subdirectory(third_party/soxr)

# -----------------------------------------------------------------------------
# first_party/audio_engine — capture backends
# -----------------------------------------------------------------------------
if (WIN32)
    add_subdirectory(first_party/audio_engine/scripts/windows ae_windows)
else()
    set(AUDIO_ENGINE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    add_subdirectory(first_party/audio_engine/scripts/linux ae_linux)
endif()

# -----------------------------------------------------------------------------
# whisper_core — portable app core (zero UI deps)
# -----------------------------------------------------------------------------
add_library(whisper_core STATIC
    core/settings.cpp
)

target_include_directories(whisper_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(whisper_core PUBLIC
    whisper
    soxr
)

# -----------------------------------------------------------------------------
# tests
# -----------------------------------------------------------------------------
add_executable(whisper_tests
    tests/test_main.cpp
    tests/settings_test.cpp
)
target_link_libraries(whisper_tests PRIVATE whisper_core)
