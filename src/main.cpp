#include <windows.h>
#include <objbase.h>

#include "window.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // STA en el hilo UI — necesario para usar IMMDeviceEnumerator / IMMNotificationClient.
    // El hilo de captura (en wasapi_capture.cpp) sigue usando MTA en su propio hilo:
    // apartments distintos en hilos distintos es válido.
    HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HWND main_wnd = window_create(hInstance);
    if (!main_wnd) {
        MessageBoxW(nullptr, L"No se pudo crear la ventana principal.",
                    L"whisper_destilado", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(com_hr)) CoUninitialize();
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(main_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (SUCCEEDED(com_hr)) CoUninitialize();
    return (int) msg.wParam;
}
