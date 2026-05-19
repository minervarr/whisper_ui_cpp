#include "language_combo.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "whisper.h"

namespace ui {

namespace {

// Tabla estática inicializada una sola vez. Importante: para que los punteros
// `code` y `label.c_str()` permanezcan válidos durante toda la vida del proceso
// (los guardamos como ITEM_DATA en los combos), la tabla NO puede reasignarse
// nunca. Por eso es un `static const std::vector` construido vía lambda; tras
// la construcción nadie la muta.
struct LangItem {
    const char * code;   // puntero estable (whisper interno o literal "auto")
    std::wstring label;  // "Spanish (es)"  — vive en este vector estático
};

std::wstring utf8_to_wide(const char * s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::wstring title_case(std::wstring s) {
    if (!s.empty()) s[0] = (wchar_t) towupper(s[0]);
    return s;
}

const std::vector<LangItem> & language_table() {
    static const std::vector<LangItem> table = [] {
        std::vector<LangItem> v;
        v.reserve(128);  // ~99 idiomas + auto; reserve previene cualquier realloc

        v.push_back({"auto", L"Auto-detectar idioma"});

        const int n = whisper_lang_max_id();
        for (int i = 0; i <= n; ++i) {
            const char * code = whisper_lang_str(i);       // puntero estático interno
            const char * full = whisper_lang_str_full(i);
            if (!code) continue;
            std::wstring label = full ? title_case(utf8_to_wide(full))
                                      : utf8_to_wide(code);
            label += L" (";
            label += utf8_to_wide(code);
            label += L")";
            v.push_back({code, std::move(label)});
        }
        return v;
    }();
    return table;
}

} // namespace

void populate_language_combo(HWND combo) {
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    const auto & table = language_table();
    for (const auto & item : table) {
        int idx = (int) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM) item.label.c_str());
        // item.code es un puntero estable (whisper interno o literal); seguro como ITEM_DATA.
        SendMessageW(combo, CB_SETITEMDATA, idx, (LPARAM) item.code);
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

const char * get_selected_language_code(HWND combo) {
    if (!combo) return "auto";
    int idx = (int) SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return "auto";
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, idx, 0);
    return data ? reinterpret_cast<const char *>(data) : "auto";
}

void set_selected_language_code(HWND combo, const char * code) {
    if (!combo || !code) return;
    int count = (int) SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        LRESULT data = SendMessageW(combo, CB_GETITEMDATA, i, 0);
        const char * c = reinterpret_cast<const char *>(data);
        if (c && std::strcmp(c, code) == 0) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

} // namespace ui
