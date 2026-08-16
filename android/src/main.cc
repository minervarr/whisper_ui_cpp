#include <android_native_app_glue.h>
#include <android/log.h>

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#include "android_host.hh"  // app_shell: the Android Host
#include "gui/app.h"

#define LOG_TAG "WhisperDestilado"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// The Android entry point, and the sibling of gui/main.cc (which defines
// app_shell_main for the desktop hosts). Construct the app, hand it a Host, run
// it. Everything below create() is the same gui/app.cpp the desktop runs —
// there is no Android build of the UI.
//
// ── Why this function is so loud ─────────────────────────────────────────────
//
// When android_main() RETURNS, the activity is finished — so every failure
// here, however it happens, looks identical from the outside: the app opens
// and closes. There is no window left to put an error in and no console to
// print one to. The phase lines below are the only way to tell "create()
// refused" from "something threw" from "it ran and quit". Filter with:
//
//     adb logcat -s WhisperDestilado:V AndroidHost:V whisper_destilado:V
void android_main(android_app* state) {
    // Point every getenv()-driven path at the app's private dir before ANY of
    // the app's code constructs. internalDataPath is a plain field on
    // ANativeActivity — no JNI needed.
    //   XDG_CONFIG_HOME / HOME  -> cfg::config_dir() (settings) and the
    //                              default save path.
    //   WHISPER_MODEL_DIR        -> resolve_model()'s "scan a folder instead
    //                              of <exe_dir>/models" branch; the user pushes
    //                              a ggml model into this directory.
    const char* files = state->activity->internalDataPath;
    if (!files || !*files) files = "/data/data/io.nava.whisper_destilado/files";
    setenv("XDG_CONFIG_HOME", files, 1);
    setenv("HOME", files, 1);
    setenv("WHISPER_MODEL_DIR", (std::string(files) + "/models").c_str(), 1);

    LOGI("phase 1/5: entry -- constructing App");
    try {
        gui::App app;

        LOGI("phase 2/5: create() -- host init, Vulkan, fonts, model loader");
        // No launch argument, and no all-files access: every file this app
        // reads (the ggml model) and writes (settings, transcripts) lives in
        // internalDataPath, so the MANAGE_EXTERNAL_STORAGE prompt Matrix
        // Player needs would be a Settings screen this app cannot even use.
        if (!app.create(std::make_unique<AndroidHost>(state, nullptr, nullptr,
                                                      /*requestAllFilesAccess=*/false))) {
            // create() logs its own reason first (Host::showErrorMessage is a
            // logcat line here); this only says which stage refused.
            LOGE("phase 2/5 FAILED: create() returned false -- the activity "
                 "will now finish. The line above this one is the reason.");
            return;
        }

        LOGI("phase 3/5: create() OK -- entering run()");
        app.run();

        LOGI("phase 4/5: run() returned -- shutting down");
        app.shutdown();
        LOGI("phase 5/5: clean exit");
    } catch (const std::exception& e) {
        // An exception escaping android_main() is otherwise a bare abort with
        // no message attached to it.
        LOGE("FATAL: unhandled exception: %s", e.what());
    } catch (...) {
        LOGE("FATAL: unhandled non-standard exception");
    }
}
