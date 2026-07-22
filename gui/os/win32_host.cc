// Win32 skin of the GUI (host.hh): one normal resizable window + message-loop
// pump, on vk_canvas's raw Win32 backend (win32_platform.cc from the
// framework). NOT built or verified on this machine — verify on the
// Windows PC.

#include "../host.hh"

#include "win32_platform.hh" // engine Win32 seams (from framework/vk_canvas)

#include <cstdio>
#include <memory>

namespace {

HWND        g_hwnd  = nullptr;
FrameInput* g_input = nullptr; // valid only inside pump()
bool        g_quit  = false;
bool        g_dirty = false;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (hwnd == g_hwnd && g_input &&
        win32_translate_input(hwnd, msg, wp, lp, *g_input)) {
        g_dirty = true; // any input can change what's on screen
        return 0;
    }

    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            if (hwnd == g_hwnd) {
                g_quit = true;
                PostQuitMessage(0);
            }
            return 0;
        case WM_SIZE:
        case WM_PAINT:
        case WM_SHOWWINDOW:
            // Resize/expose: repaint on the next loop pass.
            g_dirty = true;
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

class Win32Host : public gui::AppHost {
public:
    bool init() override
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = wnd_proc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"whisper_destilado";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);

        g_hwnd = CreateWindowExW(0, L"whisper_destilado", L"whisper_destilado",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 960, 700,
                                 nullptr, nullptr, GetModuleHandleW(nullptr),
                                 nullptr);
        if (!g_hwnd) {
            std::fprintf(stderr, "[x] window creation failed\n");
            return false;
        }
        SetForegroundWindow(g_hwnd);
        surface_  = std::make_unique<Win32SurfaceProvider>(g_hwnd);
        renderer_ = std::make_unique<Renderer>(*surface_, assets_);
        return true;
    }

    AssetReader & assets() override { return assets_; }
    Renderer * renderer() override { return renderer_.get(); }

    void pump(int timeout_ms, FrameInput & input) override
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD) timeout_ms,
                                  QS_ALLINPUT);
        g_input = &input;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        g_input = nullptr;
    }

    bool quit_requested() override { return g_quit; }
    bool take_dirty() override { bool d = g_dirty; g_dirty = false; return d; }

    void copy_text(const std::string & utf8) override
    {
        if (!OpenClipboard(g_hwnd)) return;
        EmptyClipboard();
        // UTF-8 -> UTF-16 into a movable global for CF_UNICODETEXT.
        int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (wn > 0) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T) wn * sizeof(wchar_t));
            if (h) {
                wchar_t * dst = (wchar_t *) GlobalLock(h);
                MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, dst, wn);
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);   // clipboard owns h now
            }
        }
        CloseClipboard();
    }

private:
    FileAssetReader assets_;
    std::unique_ptr<Win32SurfaceProvider> surface_;
    std::unique_ptr<Renderer>             renderer_;
};

} // namespace

namespace gui {
std::unique_ptr<AppHost> make_host() { return std::make_unique<Win32Host>(); }
} // namespace gui
