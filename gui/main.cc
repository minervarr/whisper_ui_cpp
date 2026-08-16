// whisper_destilado — cross-platform Vulkan GUI entry point.
// Flags: --selftest (headless model+transcription proof, CI-able).
//
// This file DEFINES app_shell_main(); the real main()/WinMain() belongs to the
// platform host (app_shell's wayland/win32 hosts), which runs platform
// bootstrap first and then calls this — see framework/app_shell/app_main.hh.

#include <cstring>

#include "app_main.hh"   // app_shell: the entry point this file defines
#include "gui/app.h"

int app_shell_main(int argc, char ** argv)
{
    bool selftest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0) selftest = true;

    gui::App app;
    if (selftest) return app.run_selftest();

    if (!app.create()) return 1;
    app.run();
    return 0;
}
