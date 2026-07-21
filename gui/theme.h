#pragma once
// whisper_destilado's visual identity: dark studio look built on the engine's
// palette, plus the historical LED state colors (carried from led_control.cpp
// through RecorderController::led_color()'s 0x00BBGGRR packing).

#include <cstdint>

#include "canvas.hh"

namespace gui {

namespace pal {
constexpr Color bg      = col::bg;
constexpr Color panel   = col::panel;
constexpr Color panel2  = col::panel2;
constexpr Color text    = col::text;
constexpr Color dim     = col::dim;
constexpr Color accent  = col::accent;
constexpr Color track   = col::track;
constexpr Color red     = col::red;
constexpr Color green   = col::green;
constexpr Color btnIdle = col::btnIdle;
constexpr Color btnRec  = col::btnRec;
} // namespace pal

// One type scale — consistent rhythm at every window size.
struct TypeScale {
    float title, body, small, big;
    explicit TypeScale(float h)
        : title(h * 0.032f), body(h * 0.024f), small(h * 0.018f),
          big(h * 0.040f) {}
};

struct PointerState {
    float x = -1, y = -1;
    bool  down = false;
};

// 0x00BBGGRR (inference::rgb / RecorderController::led_color) -> engine Color.
inline Color led_to_color(uint32_t packed)
{
    return { (packed & 0xFF) / 255.0f,
             ((packed >> 8) & 0xFF) / 255.0f,
             ((packed >> 16) & 0xFF) / 255.0f,
             1.0f };
}

inline Color with_alpha(Color c, float a)
{
    c.a *= a;
    return c;
}

} // namespace gui
