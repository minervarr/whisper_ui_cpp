#pragma once
// The GUI's platform seam (scanersito pattern, single-window subset):
// gui/app.cpp is the portable skeleton (controller, frame loop, drawing);
// an AppHost is the per-OS skin (window, event pump). One implementation
// per platform in os/ — os/wayland_host.cc and os/win32_host.cc — each
// defining make_host().
//
// Contract: the skeleton calls input.beginFrame() and then
// pump(timeout_ms, input), which sleeps until events/timeout and feeds the
// window's input into the FrameInput.

#include "frame_input.hh"
#include "platform.hh"
#include "renderer.hh"

#include <memory>
#include <string>

namespace gui {

class AppHost {
public:
    virtual ~AppHost() = default;

    // Creates the window + its Renderer. False = fatal (no display).
    virtual bool init() = 0;

    virtual AssetReader & assets() = 0;

    virtual Renderer * renderer() = 0;

    // Sleep up to timeout_ms for window-system events, dispatch them into
    // `input`. Call input.beginFrame() before this, every frame.
    virtual void pump(int timeout_ms, FrameInput & input) = 0;

    // The window was closed by the system (compositor/user).
    virtual bool quit_requested() = 0;

    // True once when the window system wants a repaint (resize/expose).
    virtual bool take_dirty() = 0;

    // Put UTF-8 text on the system clipboard.
    virtual void copy_text(const std::string & utf8) = 0;
};

std::unique_ptr<AppHost> make_host();   // defined by the platform skin in os/

} // namespace gui
