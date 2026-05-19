#include "led_control.h"

#include <windows.h>

namespace {

LedState g_state = LedState::Idle;

COLORREF state_color(LedState s) {
    switch (s) {
        case LedState::Idle:          return RGB(120, 120, 120);
        case LedState::Ready:         return RGB( 60, 160,  60);
        case LedState::RecordingOk:   return RGB( 40, 220,  40);
        case LedState::RecordingWarn: return RGB(240, 200,  40);
        case LedState::RecordingClip: return RGB(230,  40,  40);
        case LedState::Processing:    return RGB( 40, 120, 230);
        case LedState::Error:         return RGB(150,  40,  40);
    }
    return RGB(120, 120, 120);
}

} // namespace

void led_set_state(HWND hwnd_led, LedState state) {
    if (state == g_state) return;
    g_state = state;
    if (hwnd_led) {
        InvalidateRect(hwnd_led, nullptr, TRUE);
    }
}

void led_on_draw_item(const DRAWITEMSTRUCT * dis) {
    if (!dis) return;

    const RECT & r = dis->rcItem;
    HDC hdc = dis->hDC;

    // Background — same as parent dialog face.
    HBRUSH bg = (HBRUSH) GetSysColorBrush(COLOR_BTNFACE);
    FillRect(hdc, &r, bg);

    // Circle inscribed in the control rect with a 1px margin.
    COLORREF c = state_color(g_state);
    HBRUSH fill = CreateSolidBrush(c);
    HPEN   pen  = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
    HBRUSH old_brush = (HBRUSH) SelectObject(hdc, fill);
    HPEN   old_pen   = (HPEN)   SelectObject(hdc, pen);

    Ellipse(hdc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(fill);
    DeleteObject(pen);
}
