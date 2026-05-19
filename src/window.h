#pragma once

#include <windows.h>

// Crea la ventana principal (dialog modeless) y devuelve su HWND.
// Devuelve nullptr si falla.
HWND window_create(HINSTANCE hInstance);
