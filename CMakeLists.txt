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

# -----------------------------------------------------------------------------
# WHISPER_ARCH_LEVEL -- x86-64 microarch level for ggml's CPU kernels (mel
# spectrogram, resampling, and any op without a Vulkan path; GPU inference
# itself is unaffected since that runs through GGML_VULKAN above).
#   ""          -- default: same as "v3" below (this is what the project has
#                  always shipped; kept as the unset default so a plain
#                  `cmake -S . -B build` behaves exactly as before).
#   "universal" -- true x86-64 baseline (SSE2 only), runs on any 64-bit x86 CPU.
#   "v3"        -- AVX2/BMI2/FMA/F16C (Haswell 2013+, Zen 2+).
#   "v4"        -- v3 plus AVX-512F (Skylake-X/Ice Lake+, Zen 4+).
#   anything else -- treated as a raw -march=<value> (e.g. "znver4", "native")
#                  passed straight to the compiler, with ggml's own feature
#                  detection left off so the two don't fight over flags.
# Not applied on Windows/macOS: MSVC doesn't take -march=x86-64-vN, and Apple
# builds are arm64.
# -----------------------------------------------------------------------------
set(WHISPER_ARCH_LEVEL "" CACHE STRING "x86-64 microarch level: universal, v3 (default), v4, or a raw -march value")

if (NOT WIN32 AND NOT APPLE)
    if (WHISPER_ARCH_LEVEL STREQUAL "" OR WHISPER_ARCH_LEVEL STREQUAL "v3")
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        set(GGML_SSE42  ON  CACHE BOOL "" FORCE)
        set(GGML_AVX    ON  CACHE BOOL "" FORCE)
        set(GGML_AVX2   ON  CACHE BOOL "" FORCE)
        set(GGML_BMI2   ON  CACHE BOOL "" FORCE)
        set(GGML_FMA    ON  CACHE BOOL "" FORCE)
        set(GGML_F16C   ON  CACHE BOOL "" FORCE)
        set(GGML_AVX512 OFF CACHE BOOL "" FORCE)
    elseif (WHISPER_ARCH_LEVEL STREQUAL "universal")
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        set(GGML_SSE42  OFF CACHE BOOL "" FORCE)
        set(GGML_AVX    OFF CACHE BOOL "" FORCE)
        set(GGML_AVX2   OFF CACHE BOOL "" FORCE)
        set(GGML_BMI2   OFF CACHE BOOL "" FORCE)
        set(GGML_FMA    OFF CACHE BOOL "" FORCE)
        set(GGML_F16C   OFF CACHE BOOL "" FORCE)
        set(GGML_AVX512 OFF CACHE BOOL "" FORCE)
    elseif (WHISPER_ARCH_LEVEL STREQUAL "v4")
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        set(GGML_SSE42  ON  CACHE BOOL "" FORCE)
        set(GGML_AVX    ON  CACHE BOOL "" FORCE)
        set(GGML_AVX2   ON  CACHE BOOL "" FORCE)
        set(GGML_BMI2   ON  CACHE BOOL "" FORCE)
        set(GGML_FMA    ON  CACHE BOOL "" FORCE)
        set(GGML_F16C   ON  CACHE BOOL "" FORCE)
        set(GGML_AVX512 ON  CACHE BOOL "" FORCE)
    elseif (WHISPER_ARCH_LEVEL STREQUAL "native")
        set(GGML_NATIVE ON CACHE BOOL "" FORCE)
    else()
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        add_compile_options(-march=${WHISPER_ARCH_LEVEL})
    endif()
endif()

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
# The engine's desktop build is its root CMakeLists.txt (scripts/<os> are now
# thin launchers). We only want the capture backends, not the FLAC/MP3/DSD
# decoders or the smoke-test tools.
set(AUDIO_ENGINE_BUILD_DECODERS OFF CACHE BOOL "" FORCE)
set(AUDIO_ENGINE_BUILD_TOOLS   OFF CACHE BOOL "" FORCE)
add_subdirectory(first_party/audio_engine ae_engine)

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
)

if (WIN32)
    # WASAPI mic + direct USB (libusb) capture.
    target_sources(whisper_core PRIVATE
        core/audio/capture_wasapi.cpp
        core/audio/capture_usb.cpp
    )
elseif(ANDROID)
    # NDK AAudio mic capture. USB DAC capture is desktop-only for now.
    target_sources(whisper_core PRIVATE core/audio/capture_aaudio.cpp)
else()
    target_sources(whisper_core PRIVATE
        core/audio/capture_alsa.cpp
        core/audio/capture_jack.cpp
        core/audio/capture_usb.cpp
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
    # WASAPI + COM device enumeration + MMCSS for the capture backend.
    target_link_libraries(whisper_core PUBLIC
        ae_usb
        ole32 propsys mmdevapi avrt uuid
    )
elseif(ANDROID)
    # NDK's own AAudio (API 26+). audio_engine's ae_aaudio is a sink only, so
    # the capture side links the platform lib directly.
    target_link_libraries(whisper_core PUBLIC aaudio)
else()
    # ae_* targets export their include dirs (usb_audio.h, alsa_source.h,
    # jack_source.h) PUBLIC-ly.
    target_link_libraries(whisper_core PUBLIC
        ae_usb
        ae_alsa
    )
    if (TARGET ae_jack)
        target_link_libraries(whisper_core PUBLIC ae_jack)
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
# compiler comes from the shader-slang package.
if (NOT WIN32 AND NOT DEFINED CACHE{VCE_SLANGC} AND EXISTS /opt/shader-slang/bin/slangc)
    set(VCE_SLANGC /opt/shader-slang/bin/slangc CACHE FILEPATH "slangc")
endif()

add_subdirectory(framework/vk_canvas/core vk_canvas_core)

# -----------------------------------------------------------------------------
# app_shell — the Host/AppView seam and its platform hosts. Must come before
# the gui executable: gui/app.{h,cpp} include app_shell's headers and link it.
# -----------------------------------------------------------------------------
# The names this application tells its shell (preserve the existing window
# class, app_id and title; APP_SHELL_APP_NAME names the log file and Android
# logcat tag). Default APP_SHELL_STATE_HOME (empty) keeps everything the app
# WRITES beside the executable — the same self-contained build tree as before.
set(APP_SHELL_APP_NAME      "whisper_destilado"   CACHE STRING "" FORCE)
set(APP_SHELL_WIN_CLASS     "WhisperDestiladoMain" CACHE STRING "" FORCE)
set(APP_SHELL_WINDOW_TITLE  "whisper_destilado"   CACHE STRING "" FORCE)
set(APP_SHELL_APP_ID        "whisper_destilado"   CACHE STRING "" FORCE)
add_subdirectory(framework/app_shell app_shell_build)

# The build-time text-size floor generator needs OUR font list (it bakes the
# worst case across the faces the app actually loads).
include(AppShellMinTextSize)
app_shell_generate_min_text_size(
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NewCM10-Book.otf)

# app_shell's ui_metrics.cc includes the header that call generated; the
# consumer owns the font list, so the consumer owns this dependency too.
add_dependencies(app_shell generate_ui_min_text_size)

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
        framework/vk_canvas/platform/windows/win32_platform.cc
        resources/app.rc)
else()
    add_subdirectory(framework/vk_canvas/platform/linux vk_canvas_wayland)
    set(_gui_host_sources
        framework/vk_canvas/platform/linux/wayland_platform.cc)
endif()

# Shared GUI drawing code — the pure views/controller/fonts, no window, no
# frame loop. Both the live app and the debug UI-capture tool link this, so
# neither can drift from the other's rendering.
add_library(whisper_gui_lib STATIC
    gui/views.cpp
    gui/recorder_controller.cpp
    gui/fonts.cpp
)
target_link_libraries(whisper_gui_lib PUBLIC whisper_core vk_canvas_core)

add_executable(whisper_destilado
    gui/main.cc
    gui/app.cpp
    ${_gui_host_sources}
)
add_dependencies(whisper_destilado
    gui_shared_shaders gui_shaders generate_ui_min_text_size)

target_link_libraries(whisper_destilado PRIVATE whisper_gui_lib)

if (WIN32)
    target_include_directories(whisper_destilado PRIVATE
        framework/vk_canvas/platform/windows)
    target_link_libraries(whisper_destilado PRIVATE
        app_shell app_shell_win32 user32)
    if (MSVC)
        target_compile_options(whisper_destilado PRIVATE /utf-8)
        # The manifest is embedded via resources/app.rc (resource id 1, type
        # 24); stop link.exe auto-embedding a second one (CVT1100).
        target_link_options(whisper_destilado PRIVATE /MANIFEST:NO)
    endif()
else()
    target_link_libraries(whisper_destilado PRIVATE
        app_shell app_shell_wayland vk_canvas_wayland)
endif()

# Fonts next to the exe (FileAssetReader reads <exe>/assets/), like shaders.
add_custom_command(TARGET whisper_destilado POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/assets/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NewCM10-Book.otf
        ${CMAKE_BINARY_DIR}/assets/fonts/
)

# -----------------------------------------------------------------------------
# whisper_ui_capture — headless PNG snapshots of every UI state (DEBUG ONLY).
# Not present in Release builds (like scanersito's two_window_spike). Reads the
# same assets/ dir (shaders + font) the app does, so it needs them built first.
# -----------------------------------------------------------------------------
if (NOT WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_subdirectory(framework/vk_canvas/core/capture vk_canvas_capture)

    add_executable(whisper_ui_capture tools/ui_capture/main.cc)
    add_dependencies(whisper_ui_capture gui_shared_shaders gui_shaders)
    target_link_libraries(whisper_ui_capture PRIVATE
        whisper_gui_lib vk_canvas_capture vk_canvas_wayland)

    # Font next to this exe too (same assets/ dir as the app, but make the
    # capture tool self-sufficient when built alone).
    add_custom_command(TARGET whisper_ui_capture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/assets/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NewCM10-Book.otf
            ${CMAKE_BINARY_DIR}/assets/fonts/
    )
endif()
