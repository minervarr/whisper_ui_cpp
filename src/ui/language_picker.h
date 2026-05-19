#pragma once

#include <windows.h>

#include <string>

namespace ui {

// Diálogo modal con un combo de idiomas. Devuelve el código corto del idioma
// elegido ("es", "en", "auto"). Cadena vacía si se canceló.
std::string pick_language(HWND parent, const std::string & initial);

} // namespace ui
