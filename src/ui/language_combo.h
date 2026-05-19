#pragma once

#include <windows.h>

namespace ui {

// Llena el combo con "Auto-detectar" + todos los idiomas reportados por whisper.cpp
// (whisper_lang_max_id + whisper_lang_str + whisper_lang_str_full).
// Almacena el código corto ("es", "en", ...) en CB_SETITEMDATA.
void populate_language_combo(HWND combo);

// Código corto del item seleccionado actualmente (o "auto").
const char * get_selected_language_code(HWND combo);

// Selecciona el ítem cuyo código corto coincide; fallback a "auto" si no se halla.
void set_selected_language_code(HWND combo, const char * code);

} // namespace ui
