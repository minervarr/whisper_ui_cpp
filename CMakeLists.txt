cmake_minimum_required(VERSION 3.20)
project(whisper_destilado LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -----------------------------------------------------------------------------
# whisper.cpp dependency (git submodule at whisper.cpp/)
#
# Para actualizar: cd whisper.cpp && git pull && cd .. && git add whisper.cpp
# Primera compilación tras un clone: build.bat ejecuta git submodule update --init
# -----------------------------------------------------------------------------

# Force options BEFORE add_subdirectory — whisper.cpp defaults
# BUILD_SHARED_LIBS=ON on MSVC, which generaría DLLs.
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER    OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN             ON  CACHE BOOL "" FORCE)
set(GGML_AVX2               ON  CACHE BOOL "" FORCE)

# Windows MAX_PATH workaround:
# La subcompilación de vulkan-shaders-gen crea paths largos; ponemos el
# binary-dir de whisper.cpp en un path corto bajo el perfil del usuario.
if (WIN32)
    if (DEFINED ENV{USERPROFILE})
        set(_wd_short_base "$ENV{USERPROFILE}/wd_d")
    else()
        set(_wd_short_base "${CMAKE_BINARY_DIR}/d")
    endif()
    add_subdirectory(whisper.cpp "${_wd_short_base}/w-build")
else()
    add_subdirectory(whisper.cpp)
endif()

add_executable(whisper_destilado WIN32
    src/main.cpp
    src/window.cpp
    src/led_control.cpp
    src/config.cpp
    src/audio/wasapi_capture.cpp
    src/audio/device_enum.cpp
    src/audio/device_notify.cpp
    src/audio/file_reader.cpp
    src/inference/model_loader.cpp
    src/inference/transcribe.cpp
    src/inference/confidence.cpp
    src/inference/output_format.cpp
    src/ui/result_dialog.cpp
    src/ui/settings_dlg.cpp
    src/ui/language_combo.cpp
    src/ui/language_picker.cpp
    src/ui/mic_picker.cpp
    resources/app.rc
)

target_include_directories(whisper_destilado PRIVATE src)

target_link_libraries(whisper_destilado PRIVATE
    whisper          # ggml/ggml-vulkan/ggml-cpu vienen transitivos
    ole32 propsys mmdevapi avrt
    comctl32 comdlg32 gdi32 user32 shell32 winmm advapi32 uuid
    mfplat mfreadwrite mfuuid
)

if (MSVC)
    target_compile_options(whisper_destilado PRIVATE /arch:AVX2 /O2 /EHsc /W4 /permissive- /utf-8)
    target_compile_definitions(whisper_destilado PRIVATE
        UNICODE _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    # The manifest is embedded via resources/app.rc (resource id 1, type 24).
    # Disable CMake/link.exe auto-embedding a second manifest to avoid CVT1100 duplicate resource.
    target_link_options(whisper_destilado PRIVATE /MANIFEST:NO)
endif()

set_target_properties(whisper_destilado PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
