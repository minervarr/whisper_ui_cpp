#include "mic_picker.h"
#include "resource.h"
#include "../audio/device_enum.h"

#include <windows.h>

#include <string>
#include <vector>

namespace ui {

namespace {

// Estado por-invocación. Los Dialog APIs son síncronos (DialogBoxParamW bloquea),
// así que un solo set de globals por hilo basta — no hay reentrancia.
struct PickerState {
    std::wstring initial_id;
    std::wstring result_id;
    bool         accepted = false;
    const wchar_t * title_override = nullptr;
    // Vector estable con los ids; sus c_str() se almacenan como CB_SETITEMDATA.
    // reserve() antes de poblar evita cualquier realloc → punteros válidos
    // hasta que el diálogo cierra. Mismo patrón que language_combo.cpp:39-61.
    std::vector<std::wstring> ids;
};

PickerState g_state;

void populate(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    auto devices = audio::enumerate_capture_devices();

    g_state.ids.clear();
    g_state.ids.reserve(devices.size() + 1);

    // Ítem 0: usar default del sistema.
    g_state.ids.push_back(L"");  // sentinela para "default"
    int idx = (int) SendMessageW(combo, CB_ADDSTRING, 0,
                                 (LPARAM) L"(Usar el predeterminado del sistema)");
    SendMessageW(combo, CB_SETITEMDATA, idx, (LPARAM) g_state.ids.back().c_str());

    int selected_index = 0;

    for (const auto & d : devices) {
        g_state.ids.push_back(d.id);
        std::wstring label = d.friendly_name.empty() ? L"(sin nombre)" : d.friendly_name;
        if (d.is_default) label += L"  (predeterminado)";
        idx = (int) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM) label.c_str());
        SendMessageW(combo, CB_SETITEMDATA, idx, (LPARAM) g_state.ids.back().c_str());

        if (!g_state.initial_id.empty() && d.id == g_state.initial_id) {
            selected_index = idx;
        }
    }

    SendMessageW(combo, CB_SETCURSEL, selected_index, 0);
}

std::wstring read_selected(HWND combo) {
    int idx = (int) SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return {};
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, idx, 0);
    if (!data) return {};
    return std::wstring(reinterpret_cast<const wchar_t *>(data));
}

INT_PTR CALLBACK PickerProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
        case WM_INITDIALOG: {
            if (g_state.title_override) {
                SetWindowTextW(hdlg, g_state.title_override);
            }
            populate(GetDlgItem(hdlg, IDC_PICK_MIC));
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                    g_state.result_id = read_selected(GetDlgItem(hdlg, IDC_PICK_MIC));
                    g_state.accepted  = true;
                    EndDialog(hdlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    g_state.result_id.clear();
                    g_state.accepted  = false;
                    EndDialog(hdlg, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            g_state.result_id.clear();
            g_state.accepted  = false;
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

} // namespace

bool show_mic_picker(HWND parent,
                     const std::wstring & initial_id,
                     std::wstring & out_id,
                     const wchar_t * title_override) {
    g_state = PickerState{};
    g_state.initial_id     = initial_id;
    g_state.title_override = title_override;

    DialogBoxParamW(GetModuleHandleW(nullptr),
                    MAKEINTRESOURCEW(IDD_MIC_PICKER),
                    parent, PickerProc, 0);

    if (g_state.accepted) {
        out_id = g_state.result_id;
        return true;
    }
    return false;
}

} // namespace ui
