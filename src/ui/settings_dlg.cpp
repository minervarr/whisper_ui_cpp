#include "settings_dlg.h"
#include "resource.h"
#include "language_combo.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace ui {

namespace {

cfg::Settings * g_target = nullptr;

std::wstring utf8_to_wide(const std::string & s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

std::string wide_to_utf8(const std::wstring & w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

float get_float_ctl(HWND dlg, int id, float def) {
    wchar_t buf[64];
    if (GetDlgItemTextW(dlg, id, buf, 64) == 0) return def;
    return (float) _wtof(buf);
}

void set_float_ctl(HWND dlg, int id, float v) {
    wchar_t b[64];
    swprintf(b, 64, L"%g", v);
    SetDlgItemTextW(dlg, id, b);
}

int get_int_ctl(HWND dlg, int id, int def) {
    BOOL ok = FALSE;
    int v = (int) GetDlgItemInt(dlg, id, &ok, TRUE);
    return ok ? v : def;
}

void set_int_ctl(HWND dlg, int id, int v) {
    SetDlgItemInt(dlg, id, (UINT) v, TRUE);
}

bool get_check(HWND dlg, int id) {
    return IsDlgButtonChecked(dlg, id) == BST_CHECKED;
}

void set_check(HWND dlg, int id, bool v) {
    CheckDlgButton(dlg, id, v ? BST_CHECKED : BST_UNCHECKED);
}

std::string get_text_utf8(HWND dlg, int id, int max_len = 4096) {
    std::wstring buf(max_len, L'\0');
    int n = GetDlgItemTextW(dlg, id, buf.data(), max_len);
    buf.resize(n);
    return wide_to_utf8(buf);
}

void apply_settings_to_dialog(HWND hdlg, const cfg::Settings & s) {
    set_selected_language_code(GetDlgItem(hdlg, IDC_S_LANG), s.language.c_str());
    set_check    (hdlg, IDC_S_TRANSLATE,       s.translate);
    set_check    (hdlg, IDC_S_DETECT_LANG,     s.detect_language);
    set_int_ctl  (hdlg, IDC_S_THREADS,         s.n_threads);

    set_check    (hdlg, IDC_S_GREEDY,         !s.use_beam_search);
    set_check    (hdlg, IDC_S_BEAM,            s.use_beam_search);
    set_int_ctl  (hdlg, IDC_S_BEAM_SIZE,       s.beam_size);
    set_int_ctl  (hdlg, IDC_S_BEST_OF,         s.best_of);

    set_float_ctl(hdlg, IDC_S_TEMP,            s.temperature);
    set_float_ctl(hdlg, IDC_S_TEMP_INC,        s.temperature_inc);
    set_float_ctl(hdlg, IDC_S_LEN_PENALTY,     s.length_penalty);
    set_float_ctl(hdlg, IDC_S_ENTROPY,         s.entropy_thold);
    set_float_ctl(hdlg, IDC_S_LOGPROB,         s.logprob_thold);
    set_float_ctl(hdlg, IDC_S_NO_SPEECH,       s.no_speech_thold);

    set_int_ctl  (hdlg, IDC_S_N_MAX_TEXT,      s.n_max_text_ctx);
    set_int_ctl  (hdlg, IDC_S_MAX_LEN,         s.max_len);
    set_int_ctl  (hdlg, IDC_S_MAX_TOKENS,      s.max_tokens);
    set_int_ctl  (hdlg, IDC_S_AUDIO_CTX,       s.audio_ctx);

    SetDlgItemTextW(hdlg, IDC_S_PROMPT, utf8_to_wide(s.initial_prompt).c_str());
    set_check    (hdlg, IDC_S_CARRY_PROMPT,    s.carry_initial_prompt);

    set_check    (hdlg, IDC_S_SUPPRESS_BLANK,  s.suppress_blank);
    set_check    (hdlg, IDC_S_SUPPRESS_NST,    s.suppress_nst);
    set_check    (hdlg, IDC_S_SINGLE_SEG,      s.single_segment);
    set_check    (hdlg, IDC_S_SPLIT_WORD,      s.split_on_word);
    set_check    (hdlg, IDC_S_TDRZ,            s.tdrz_enable);
    set_check    (hdlg, IDC_S_PRINT_PROGRESS,  s.print_progress);
    set_check    (hdlg, IDC_S_PRINT_REALTIME,  s.print_realtime);
}

void read_dialog_to_settings(HWND hdlg, cfg::Settings & s) {
    s.language        = get_selected_language_code(GetDlgItem(hdlg, IDC_S_LANG));
    s.translate       = get_check(hdlg, IDC_S_TRANSLATE);
    s.detect_language = get_check(hdlg, IDC_S_DETECT_LANG);
    s.n_threads       = get_int_ctl(hdlg, IDC_S_THREADS, 0);

    s.use_beam_search = get_check(hdlg, IDC_S_BEAM);
    s.beam_size       = get_int_ctl(hdlg, IDC_S_BEAM_SIZE, 5);
    s.best_of         = get_int_ctl(hdlg, IDC_S_BEST_OF,   5);

    s.temperature     = get_float_ctl(hdlg, IDC_S_TEMP,        0.0f);
    s.temperature_inc = get_float_ctl(hdlg, IDC_S_TEMP_INC,    0.2f);
    s.length_penalty  = get_float_ctl(hdlg, IDC_S_LEN_PENALTY, -1.0f);
    s.entropy_thold   = get_float_ctl(hdlg, IDC_S_ENTROPY,     2.4f);
    s.logprob_thold   = get_float_ctl(hdlg, IDC_S_LOGPROB,     -1.0f);
    s.no_speech_thold = get_float_ctl(hdlg, IDC_S_NO_SPEECH,   0.6f);

    s.n_max_text_ctx = get_int_ctl(hdlg, IDC_S_N_MAX_TEXT, 16384);
    s.max_len        = get_int_ctl(hdlg, IDC_S_MAX_LEN,    0);
    s.max_tokens     = get_int_ctl(hdlg, IDC_S_MAX_TOKENS, 0);
    s.audio_ctx      = get_int_ctl(hdlg, IDC_S_AUDIO_CTX,  0);

    s.initial_prompt        = get_text_utf8(hdlg, IDC_S_PROMPT);
    s.carry_initial_prompt  = get_check(hdlg, IDC_S_CARRY_PROMPT);

    s.suppress_blank = get_check(hdlg, IDC_S_SUPPRESS_BLANK);
    s.suppress_nst   = get_check(hdlg, IDC_S_SUPPRESS_NST);
    s.single_segment = get_check(hdlg, IDC_S_SINGLE_SEG);
    s.split_on_word  = get_check(hdlg, IDC_S_SPLIT_WORD);
    s.tdrz_enable    = get_check(hdlg, IDC_S_TDRZ);
    s.print_progress = get_check(hdlg, IDC_S_PRINT_PROGRESS);
    s.print_realtime = get_check(hdlg, IDC_S_PRINT_REALTIME);
}

INT_PTR CALLBACK SettingsProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
        case WM_INITDIALOG: {
            populate_language_combo(GetDlgItem(hdlg, IDC_S_LANG));
            if (g_target) apply_settings_to_dialog(hdlg, *g_target);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_S_RESTORE: {
                    cfg::Settings def = cfg::Settings::fast_defaults();
                    apply_settings_to_dialog(hdlg, def);
                    return TRUE;
                }

                case IDOK:
                    if (g_target) {
                        read_dialog_to_settings(hdlg, *g_target);
                        cfg::save_settings(*g_target);
                    }
                    EndDialog(hdlg, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hdlg, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

} // namespace

bool show_settings_dialog(HWND parent, cfg::Settings & settings) {
    g_target = &settings;
    INT_PTR rc = DialogBoxParamW(GetModuleHandleW(nullptr),
                                 MAKEINTRESOURCEW(IDD_SETTINGS),
                                 parent, SettingsProc, 0);
    g_target = nullptr;
    return rc == IDOK;
}

} // namespace ui
