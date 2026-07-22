// Wayland skin of the GUI (host.hh), on vk_canvas's raw-Wayland backend
// (framework/vk_canvas/platform/linux/): wayland-client + xkbcommon,
// xdg-shell — no toolkits. Single windowed (non-fullscreen) utility window.

#include "../host.hh"

#include "wayland_display.hh"
#include "wayland_platform.hh"
#include "wayland_window.hh"

#include <cstdio>
#include <memory>

namespace {

constexpr const char * kAppId = "io.nava.whisper_destilado";

class WaylandHost : public gui::AppHost {
public:
    bool init() override
    {
        display_ = std::make_unique<WaylandDisplay>();
        if (!display_->valid()) {
            std::fprintf(stderr, "[x] no Wayland compositor "
                                 "(WAYLAND_DISPLAY / xdg_wm_base missing)\n");
            return false;
        }
        window_ = std::make_unique<WaylandWindow>(
            *display_, "whisper_destilado", kAppId, 960, 700);
        if (!window_->valid())
            return false;
        window_->take_resized();   // initial size is not a resize
        provider_ = std::make_unique<WaylandSurfaceProvider>(*display_, *window_);
        // Desktop on MAILBOX: 3 swapchain images (see renderer.hh's ctor note).
        renderer_ = std::make_unique<Renderer>(*provider_, assets_, 3);
        return true;
    }

    AssetReader & assets() override { return assets_; }
    Renderer * renderer() override { return renderer_.get(); }

    void pump(int timeout_ms, FrameInput & input) override
    {
        display_->set_sink(window_->surface(), &input);
        if (!display_->dispatch(timeout_ms))
            quit_ = true;   // connection died (compositor gone)

        if (window_->closed())
            quit_ = true;
        if (window_->take_resized()) {
            renderer_->notifyResized();
            dirty_ = true;
        }
    }

    bool quit_requested() override { return quit_; }
    bool take_dirty() override { bool d = dirty_; dirty_ = false; return d; }

    void copy_text(const std::string & utf8) override
    {
        display_->set_clipboard_text(utf8);
    }

private:
    FileAssetReader assets_;
    std::unique_ptr<WaylandDisplay>         display_;
    std::unique_ptr<WaylandWindow>          window_;
    std::unique_ptr<WaylandSurfaceProvider> provider_;
    std::unique_ptr<Renderer>               renderer_;

    bool quit_  = false;
    bool dirty_ = false;
};

} // namespace

namespace gui {
std::unique_ptr<AppHost> make_host() { return std::make_unique<WaylandHost>(); }
} // namespace gui
