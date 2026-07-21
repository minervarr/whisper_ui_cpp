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
    core/model_loader.cpp
    core/transcribe.cpp
    core/confidence.cpp
    core/output_format.cpp
    core/languages.cpp
    core/audio/capture_convert.cpp
    core/audio/file_reader.cpp
    core/audio/capture.cpp
    core/audio/capture_usb.cpp
)

if (WIN32)
    target_sources(whisper_core PRIVATE core/audio/capture_wasapi.cpp)
else()
    target_sources(whisper_core PRIVATE
        core/audio/capture_alsa.cpp
        core/audio/capture_jack.cpp
    )
endif()

target_include_directories(whisper_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/soxr/src
)

target_link_libraries(whisper_core PUBLIC
    whisper
    soxr
)

if (WIN32)
    target_link_libraries(whisper_core PUBLIC audio_engine_windows)
else()
    # audio_engine_* targets export their include dirs (usb_audio.h,
    # alsa_capture.h, jack_capture.h) PUBLIC-ly.
    target_link_libraries(whisper_core PUBLIC
        audio_engine_linux
        audio_engine_alsa
    )
    if (TARGET audio_engine_jack)
        target_link_libraries(whisper_core PUBLIC audio_engine_jack)
    else()
        message(FATAL_ERROR "JACK2 backend required: install jack2 dev headers (never pipewire-jack)")
    endif()
endif()

# -----------------------------------------------------------------------------
# tests
# -----------------------------------------------------------------------------
add_executable(whisper_tests
    tests/test_main.cpp
    tests/settings_test.cpp
    tests/output_format_test.cpp
    tests/event_queue_test.cpp
    tests/capture_convert_test.cpp
    tests/file_reader_test.cpp
    tests/recorder_controller_test.cpp
    gui/recorder_controller.cpp
)
target_link_libraries(whisper_tests PRIVATE whisper_core)

# -----------------------------------------------------------------------------
# cli — headless end-to-end proof
# -----------------------------------------------------------------------------
add_executable(whisper_destilado_cli cli/main.cpp)
target_link_libraries(whisper_destilado_cli PRIVATE whisper_core)

# -----------------------------------------------------------------------------
# gui — vk_canvas app (Wayland on Linux, Win32 on Windows)
# -----------------------------------------------------------------------------
# slangc for the engine's Slang->SPIR-V shader step. On this Linux setup the
# compiler comes from the shader-slang-bin package.
if (NOT WIN32 AND NOT DEFINED CACHE{VCE_SLANGC} AND EXISTS /opt/shader-slang-bin/bin/slangc)
    set(VCE_SLANGC /opt/shader-slang-bin/bin/slangc CACHE FILEPATH "slangc")
endif()

add_subdirectory(framework/vk_canvas/core vk_canvas_core)

include(framework/vk_canvas/cmake/VceShaders.cmake)
vce_compile_slang(gui_shared_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/framework/vk_canvas/first_party/vulkan_font_engine/shaders_src
    composite_vert
    composite_frag
    tiling
    coverage
    msdf_vert
    msdf_frag
)
vce_compile_slang(gui_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/framework/vk_canvas/shaders_src
    overlay_vert
    overlay_frag
    image_vert
    image_frag
    shape_vert
    shape_frag
)

if (WIN32)
    set(_gui_host_sources
        gui/os/win32_host.cc
        framework/vk_canvas/platform/windows/win32_platform.cc)
else()
    add_subdirectory(framework/vk_canvas/platform/linux vk_canvas_wayland)
    set(_gui_host_sources gui/os/wayland_host.cc)
endif()

add_executable(whisper_destilado
    gui/main.cc
    gui/app.cpp
    gui/views.cpp
    gui/recorder_controller.cpp
    ${_gui_host_sources}
)
add_dependencies(whisper_destilado gui_shared_shaders gui_shaders)

target_link_libraries(whisper_destilado PRIVATE whisper_core vk_canvas_core)

if (WIN32)
    target_include_directories(whisper_destilado PRIVATE
        framework/vk_canvas/platform/windows)
    target_link_libraries(whisper_destilado PRIVATE user32)
    if (MSVC)
        target_compile_options(whisper_destilado PRIVATE /utf-8)
    endif()
else()
    target_link_libraries(whisper_destilado PRIVATE vk_canvas_wayland)
endif()

# Fonts next to the exe (FileAssetReader reads <exe>/assets/), like shaders.
add_custom_command(TARGET whisper_destilado POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/assets/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NewCM10-Book.otf
        ${CMAKE_BINARY_DIR}/assets/fonts/
)
