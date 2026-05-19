#include "language_picker.h"
#include "language_combo.h"
#include "resource.h"

#include <windows.h>

#include <string>

namespace ui {

namespace {

std::string  g_initial;
std::string  g_result;

INT_PTR CALLBACK PickerProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
        case WM_INITDIALOG: {
            HWND combo = GetDlgItem(hdlg, IDC_PICK_LANG);
            populate_language_combo(combo);
            set_selected_language_code(combo, g_initial.empty() ? "auto" : g_initial.c_str());
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    HWND combo = GetDlgItem(hdlg, IDC_PICK_LANG);
                    g_result = get_selected_language_code(combo);
                    EndDialog(hdlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    g_result.clear();
                    EndDialog(hdlg, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            g_result.clear();
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

} // namespace

std::string pick_language(HWND parent, const std::string & initial) {
    g_initial = initial;
    g_result.clear();
    DialogBoxParamW(GetModuleHandleW(nullptr),
                    MAKEINTRESOURCEW(IDD_LANG_PICKER),
                    parent, PickerProc, 0);
    return g_result;
}

} // namespace ui
