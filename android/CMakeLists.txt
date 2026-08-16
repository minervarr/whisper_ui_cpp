cmake_minimum_required(VERSION 3.22.1)
project(whisper_destilado_android)

# Reached via app/build.gradle's externalNativeBuild.cmake.path. REPO_ROOT is
# one level up from this file (android/ is a sibling of core/, gui/,
# framework/ at the repo root).
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

# whisper.cpp: CPU-only on Android. GGML_VULKAN stays OFF (the desktop root
# CMakeLists forces it ON for the GPU path; a phone has no point — this is a
# dictation app, not a game). Nothing sets the x86 GGML_* arch flags either:
# ggml's GGML_NATIVE_DEFAULT already falls to OFF under the NDK cross-compiler
# (CMAKE_CROSSCOMPILING), so no -march leaks in from the desktop list.
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT}/third_party/whisper.cpp ${CMAKE_BINARY_DIR}/whisper)

# soxr: the resampler whisper_core's capture conversion needs. Portable C,
# add_subdirectory'd exactly as the desktop root CMakeLists does.
list(APPEND CMAKE_MODULE_PATH ${REPO_ROOT}/third_party/soxr/cmake/Modules)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP       OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT}/third_party/soxr ${CMAKE_BINARY_DIR}/soxr)

# vk_canvas_core: Vulkan canvas/text engine, unmodified. Its own CMakeLists
# has an if(ANDROID) branch linking vulkan/android/log (NDK provides libvulkan),
# so PUBLIC linking the target alone is enough.
add_subdirectory(${REPO_ROOT}/framework/vk_canvas/core ${CMAKE_BINARY_DIR}/vk_canvas_core)

# ui_min_text_size.gen.h: app_shell's ui_metrics.hh needs this build-generated
# header, but it cannot be regenerated under the NDK cross-compiler (the
# min_text_size generator runs a HOST tool). It is pure font-geometry math —
# platform-independent output — so copy the already-generated copy from an
# existing desktop build tree, exactly as Matrix_Player's android/CMakeLists
# does. Our desktop build always writes it to build/linux/generated/.
set(UI_MIN_TEXT_SIZE_SRC ${REPO_ROOT}/build/linux/generated/ui_min_text_size.gen.h)
if(NOT EXISTS ${UI_MIN_TEXT_SIZE_SRC})
    message(FATAL_ERROR
        "ui_min_text_size.gen.h not found (${UI_MIN_TEXT_SIZE_SRC}). "
        "Run a desktop build first (scripts/linux/build.sh release).")
endif()
set(UI_MIN_TEXT_SIZE_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${UI_MIN_TEXT_SIZE_DIR})
configure_file(${UI_MIN_TEXT_SIZE_SRC} ${UI_MIN_TEXT_SIZE_DIR}/ui_min_text_size.gen.h COPYONLY)

# Shaders: the SAME list the desktop root CMakeLists compiles — the MSDF pair
# included, because this app draws text (unlike vk_canvas's own android demo,
# which is missing msdf_vert/frag). Output lands in app/src/main/assets/shaders/
# so AGP packages it into the APK.
#
# VCE_SLANGC must be decided BEFORE VceShaders.cmake is included: its default
# is a Windows path, which a stale first configure caches, and FORCE keeps a
# re-run from resurrecting it.
if(EXISTS /opt/shader-slang/bin/slangc)
    set(VCE_SLANGC /opt/shader-slang/bin/slangc CACHE FILEPATH "slangc" FORCE)
endif()
set(WHISPER_ANDROID_ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/assets)
include(${REPO_ROOT}/framework/vk_canvas/cmake/VceShaders.cmake)
vce_compile_slang(compile_shared_shaders
    ${WHISPER_ANDROID_ASSET_DIR}/shaders
    ${REPO_ROOT}/framework/vk_canvas/first_party/vulkan_font_engine/shaders_src
    composite_vert
    composite_frag
    tiling
    coverage
    msdf_vert
    msdf_frag
)
vce_compile_slang(compile_vk_canvas_shaders
    ${WHISPER_ANDROID_ASSET_DIR}/shaders
    ${REPO_ROOT}/framework/vk_canvas/shaders_src
    overlay_vert
    overlay_frag
    image_vert
    image_frag
    shape_vert
    shape_frag
)

# app_shell: the Host seam + the Android host. Same name the desktop passes,
# so the logcat tag stays "whisper_destilado".
set(APP_SHELL_APP_NAME "whisper_destilado" CACHE STRING "" FORCE)
set(APP_SHELL_NATIVE_APP_GLUE_DIR ${NATIVE_APP_GLUE} CACHE PATH "" FORCE)
add_subdirectory(${REPO_ROOT}/framework/app_shell ${CMAKE_BINARY_DIR}/app_shell)

# whisper_core: the portable app core. This is the root CMakeLists.txt's
# definition, but with the Android capture backend (NDK AAudio) instead of
# WASAPI/ALSA/JACK/USB — the root file's add_subdirectory chain isn't
# Android-portable (it also builds desktop-only executables), so the few
# portable pieces are re-declared here.
add_library(whisper_core STATIC
    ${REPO_ROOT}/core/settings.cpp
    ${REPO_ROOT}/core/model_loader.cpp
    ${REPO_ROOT}/core/transcribe.cpp
    ${REPO_ROOT}/core/confidence.cpp
    ${REPO_ROOT}/core/output_format.cpp
    ${REPO_ROOT}/core/languages.cpp
    ${REPO_ROOT}/core/audio/capture_convert.cpp
    ${REPO_ROOT}/core/audio/file_reader.cpp
    ${REPO_ROOT}/core/audio/capture.cpp
    ${REPO_ROOT}/core/audio/capture_aaudio.cpp
)
target_include_directories(whisper_core PUBLIC
    ${REPO_ROOT}
    ${REPO_ROOT}/third_party
    ${REPO_ROOT}/third_party/soxr/src
)
target_link_libraries(whisper_core PUBLIC whisper soxr aaudio)

# whisper_gui_lib: shared GUI drawing code — pure views/controller/fonts, no
# window, no frame loop. Same definition as the desktop root CMakeLists.
add_library(whisper_gui_lib STATIC
    ${REPO_ROOT}/gui/views.cpp
    ${REPO_ROOT}/gui/recorder_controller.cpp
    ${REPO_ROOT}/gui/fonts.cpp
)
target_link_libraries(whisper_gui_lib PUBLIC whisper_core vk_canvas_core)

# The whole app. gui/app.cpp is the portable App; src/main.cc is the Android
# entry point (the sibling of gui/main.cc, which defines app_shell_main for the
# desktop hosts). android_platform.cc is vk_canvas's Android seam
# (AndroidSurfaceProvider / AndroidAssetReader).
add_library(whisper_destilado SHARED
    src/main.cc
    ${REPO_ROOT}/gui/app.cpp
    ${REPO_ROOT}/framework/vk_canvas/platform/android/android_platform.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c
)
add_dependencies(whisper_destilado compile_shared_shaders compile_vk_canvas_shaders)

target_include_directories(whisper_destilado PRIVATE
    ${NATIVE_APP_GLUE}
    ${REPO_ROOT}/framework/vk_canvas/platform/android
    ${UI_MIN_TEXT_SIZE_DIR}
)

target_link_libraries(whisper_destilado
    app_shell
    app_shell_android
    whisper_gui_lib
    android
    log
)

# Keep the glue's NativeActivity entry point alive against linker GC (same
# rule framework/vk_canvas/platform/android/CMakeLists.txt documents).
target_link_options(whisper_destilado PRIVATE -u ANativeActivity_onCreate)

# Bundled font next to the shaders — same POST_BUILD-on-the-native-target shape
# the desktop root CMakeLists uses for fonts (fonts are static input, unlike the
# shader compile step, so a plain POST_BUILD is fine). The Gradle-side fix in
# app/build.gradle (mergeDebugAssets/mergeReleaseAssets depending on the
# buildCMake* tasks) covers the ordering, same as it does for the shaders.
add_custom_command(TARGET whisper_destilado POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${REPO_ROOT}/assets/fonts
        ${WHISPER_ANDROID_ASSET_DIR}/fonts
)
