#pragma once

#include <windows.h>

#include "config.h"

namespace ui {

// Muestra el diálogo Avanzado de forma modal. Si el usuario pulsa Aceptar,
// `settings` se actualiza con los valores del diálogo y se persiste a disco.
// Devuelve true si se aceptó, false si se canceló.
bool show_settings_dialog(HWND parent, cfg::Settings & settings);

} // namespace ui
