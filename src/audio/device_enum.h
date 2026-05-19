#pragma once

#include <string>
#include <vector>

namespace audio {

struct Device {
    std::wstring id;             // endpoint id estable (IMMDevice::GetId)
    std::wstring friendly_name;  // PKEY_Device_FriendlyName
    bool         is_default = false;  // true si coincide con el default de eCapture/eConsole
};

// Enumera capturas en estado DEVICE_STATE_ACTIVE.
// Internamente inicializa COM si el hilo aún no lo está (RAII).
std::vector<Device> enumerate_capture_devices();

// "" si no hay default disponible.
std::wstring default_capture_id();

// true si el id sigue siendo un endpoint de captura activo.
bool device_exists(const std::wstring & id);

// "" si no se encuentra (ej. id obsoleto).
std::wstring friendly_name_for(const std::wstring & id);

} // namespace audio
