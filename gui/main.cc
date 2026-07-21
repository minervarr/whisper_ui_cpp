// whisper_destilado — cross-platform Vulkan GUI entry point.
// Flags: --selftest (headless model+transcription proof, CI-able).

#include <cstring>

#include "gui/app.h"

int main(int argc, char ** argv)
{
    bool selftest = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0) selftest = true;

    gui::App app;
    return app.run(selftest);
}
