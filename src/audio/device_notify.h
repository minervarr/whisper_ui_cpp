#pragma once

#include <windows.h>

namespace audio {

// Posteado a la ventana suscrita cuando algún dispositivo de captura cambia
// (añadido / removido / cambio de estado / cambio de default eCapture).
// WM_APP+1 ya lo usa inference::WM_TRANSCRIBE_DONE, por eso +2.
constexpr UINT WM_DEVICES_CHANGED = WM_APP + 2;

class DeviceNotifier {
public:
    DeviceNotifier() = default;
    ~DeviceNotifier();

    DeviceNotifier(const DeviceNotifier &) = delete;
    DeviceNotifier & operator=(const DeviceNotifier &) = delete;

    // Registra el callback global del enumerador. hwnd recibirá WM_DEVICES_CHANGED.
    // Devuelve false si falla CoCreateInstance o RegisterEndpointNotificationCallback.
    bool start(HWND hwnd);

    // Desregistra y libera. Idempotente.
    void stop();

private:
    struct Impl;
    Impl * impl_ = nullptr;
};

} // namespace audio
