#pragma once
// The portable application: owns model, settings, capture, controller and
// view state; runs the frame loop against an AppHost. Platform-free — the
// same App runs on the Wayland and Win32 hosts.

#include <memory>
#include <string>
#include <vector>

#include "core/audio/capture.h"
#include "core/model_loader.h"
#include "core/settings.h"
#include "gui/recorder_controller.h"
#include "gui/views.h"

namespace gui {

class App {
public:
    // Returns the process exit code. `selftest` runs the headless leg
    // (model + built-in sine through the full controller path, no window).
    int run(bool selftest);

private:
    // Side effects driven by controller actions / hits.
    std::string start_capture();          // "" ok | Spanish error
    void stop_and_transcribe();
    void retranscribe(bool quality);      // same RAM buffer, new params
    void save_result();
    void select_device(int index);
    void select_language(int index);
    void handle_hit(int action);
    void refresh_devices();

    int run_selftest();

    cfg::Settings                       settings_;
    inference::ModelLoader              loader_;
    RecorderController                  ctl_;
    std::unique_ptr<audio::CaptureBackend> capture_;
    std::shared_ptr<std::vector<float>> last_take_;   // kept for the retries

    std::vector<audio::CaptureDeviceInfo> devices_;
    int                                 device_sel_ = -1;
    int                                 lang_sel_ = 0;

    DrawState                           st_;
    std::vector<std::string>            popup_items_;
    bool                                dirty_ = true;
    bool                                running_ = true;
};

} // namespace gui
