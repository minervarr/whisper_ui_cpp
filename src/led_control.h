#pragma once

#include <windows.h>

enum class LedState {
    Idle,          // gris — modelo cargando o no listo
    Ready,         // verde tenue — listo para grabar
    RecordingOk,   // verde brillante — capturando, nivel saludable
    RecordingWarn, // amarillo — cerca de clipping
    RecordingClip, // rojo — clipping detectado
    Processing,    // azul — transcribiendo
    Error          // rojo apagado — error
};

void led_set_state(HWND hwnd_led, LedState state);
void led_on_draw_item(const DRAWITEMSTRUCT * dis);
