#pragma once

#include <windows.h>

#include <string>

namespace ui {

// Diálogo modal (IDD_MIC_PICKER). Devuelve true si el usuario aceptó y rellena out_id.
//   initial_id     — id a preseleccionar; "" preselecciona "(usar el predeterminado)".
//   title_override — si != nullptr, sustituye el caption del diálogo (usado al
//                    detectar desconexión durante grabación).
bool show_mic_picker(HWND parent,
                     const std::wstring & initial_id,
                     std::wstring & out_id,
                     const wchar_t * title_override = nullptr);

} // namespace ui
