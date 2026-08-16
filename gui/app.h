#pragma once
// The portable application: owns model, settings, capture, controller and
// view state; runs the frame loop against an app_shell Host. Platform-free —
// the same App runs on the Wayland, Win32 and Android hosts. Inherits
// FrameInputView so the hosts' callbacks land in a vk_canvas FrameInput that
// the immediate-mode drawing code reads while building each frame.

#include <memory>
#include <string>
#include <vector>

#include "core/audio/capture.h"
#include "core/model_loader.h"
#include "core/settings.h"
#include "gui/recorder_controller.h"
#include "gui/views.h"

#include "frame_input_view.hh"   // app_shell: AppView + FrameInput adapter

class Host;      // app_shell
class Renderer;  // vk_canvas

namespace gui {

class App : public FrameInputView {
public:
    // Ctor and dtor live in app.cpp so the unique_ptr members can see the
    // full Host / Renderer types there (main.cc must not instantiate them).
    App();
    ~App() override;

    // Creates the window, renderer, fonts and starts the model loader.
    // Returns false on a fatal failure (no display, Vulkan init failed).
    bool create();

    // Same, but with an injected host: Android has no ambient make_host()
    // (the app's own main.cc constructs an AndroidHost around the
    // android_app* it is handed). Passing {} is equivalent to create().
    bool create(std::unique_ptr<Host> injected);

    // The frame loop; returns when the window closes.
    void run();

    // AppView: called by a host when the window/session is going away.
    void shutdown() override;

    // AppView: the window got a new size or was exposed.
    void onHostResized() override;
    void onHostExposed() override;

    // AppView: the drawing surface can come and go (Android recreates the
    // window when the system bars are hidden; see app_view.hh). GPU state —
    // the Renderer, the swapchain, every texture, the glyph atlas — dies with
    // it; CPU state survives. onSurfaceLost() drops the Renderer,
    // onSurfaceRecreated() rebuilds it against the host's new surface.
    void onSurfaceLost() override;
    bool onSurfaceRecreated() override;

    // Headless, CI-able selftest (model + built-in sine through the full
    // controller path, no window).
    int run_selftest();

private:
    // Side effects driven by controller actions / hits.
    std::string start_capture();          // "" ok | Spanish error
    void stop_and_transcribe();
    void retranscribe(bool quality);      // same RAM buffer, new params
    void save_result();
    void copy_result();
    void select_device(int index);
    void select_language(int index);
    void handle_hit(int action);
    void refresh_devices();

    // Loads settings + restores language selection. Shared by create()
    // (windowed) and run_selftest() (headless).
    void load_state();

    std::unique_ptr<Host>               host_;
    std::unique_ptr<Renderer>           renderer_;
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
