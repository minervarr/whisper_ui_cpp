#include "result_dialog.h"
#include "resource.h"
#include "language_picker.h"
#include "inference/confidence.h"
#include "inference/output_format.h"

#include <windows.h>
#include <commdlg.h>

#include <cwchar>
#include <fstream>
#include <string>

namespace ui {

namespace {

const inference::Result * g_pending = nullptr;
ResultOutcome g_outcome;
HFONT g_tier_font = nullptr;

std::wstring utf8_to_wide(const std::string & s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

std::wstring format_mmss(int64_t ms) {
    int64_t total_s = ms / 1000;
    wchar_t buf[16];
    swprintf(buf, 16, L"%02lld:%02lld", (long long)(total_s / 60), (long long)(total_s % 60));
    return buf;
}

std::wstring build_full_text(const inference::Result & r) {
    std::wstring s;
    if (!r.error.empty()) { s = L"ERROR:\r\n" + r.error; return s; }
    if (r.segments.empty()) return L"No se detectó habla en la grabación.";
    size_t reserve = 0;
    for (const auto & seg : r.segments) reserve += seg.text.size() + 4;
    s.reserve(reserve);
    for (size_t i = 0; i < r.segments.size(); ++i) {
        s += r.segments[i].text;
        if (i + 1 < r.segments.size()) s += L"\r\n";
    }
    return s;
}

std::wstring build_tier_header(const inference::Result & r) {
    if (!r.error.empty()) return L"Error";
    wchar_t buf[128];
    swprintf(buf, 128, L"Confianza: %.0f %%  —  %s",
             r.confidence_overall * 100.0,
             inference::tier_label(r.tier));
    return buf;
}

std::wstring build_info_line(const inference::Result & r) {
    if (!r.error.empty()) return L"";
    wchar_t buf[256];
    const double secs = double(r.total_duration_ms) / 1000.0;
    swprintf(buf, 256,
        L"Duración: %.2f s   ·   Idioma: %s   ·   Segmentos: %zu",
        secs,
        r.detected_language.empty() ? L"-" : r.detected_language.c_str(),
        r.segments.size());
    return buf;
}

std::wstring build_worst_segments(const inference::Result & r) {
    if (r.worst_segments.empty()) return L"";
    std::wstring out;
    for (size_t k = 0; k < r.worst_segments.size(); ++k) {
        const auto & seg = r.segments[r.worst_segments[k]];
        out += L"[" + format_mmss(seg.t0_ms) + L" → " + format_mmss(seg.t1_ms) + L"]  ";
        wchar_t prob[32];
        swprintf(prob, 32, L"(min p = %.2f)  ", seg.min_token_p);
        out += prob;
        out += seg.text;
        if (k + 1 < r.worst_segments.size()) out += L"\r\n";
    }
    return out;
}

void ensure_tier_font() {
    if (g_tier_font) return;
    LOGFONTW lf = {};
    lf.lfHeight  = -16;
    lf.lfWeight  = FW_BOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_tier_font = CreateFontIndirectW(&lf);
}

bool copy_to_clipboard(HWND owner, const std::wstring & text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) { CloseClipboard(); return false; }
    wchar_t * p = (wchar_t *) GlobalLock(h);
    memcpy(p, text.data(), text.size() * sizeof(wchar_t));
    p[text.size()] = 0;
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
    return true;
}

// Escribe content (UTF-8) tras pedir nombre de archivo. Si el usuario cancela, devuelve false.
bool prompt_and_save(HWND owner,
                     const std::string & utf8_content,
                     const wchar_t * filter,
                     const wchar_t * default_ext,
                     const wchar_t * suggested_name) {
    wchar_t fname[MAX_PATH] = L"";
    if (suggested_name) wcsncpy_s(fname, suggested_name, _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = owner;
    ofn.lpstrFilter  = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile    = fname;
    ofn.nMaxFile     = MAX_PATH;
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt  = default_ext;

    if (!GetSaveFileNameW(&ofn)) return false;

    std::ofstream f(fname, std::ios::binary);
    if (!f) {
        MessageBoxW(owner, L"No se pudo abrir el archivo para escribir.",
                    L"whisper_destilado", MB_OK | MB_ICONERROR);
        return false;
    }
    f.write(utf8_content.data(), (std::streamsize) utf8_content.size());
    return f.good();
}

struct FormatSpec {
    const wchar_t * filter;
    const wchar_t * ext;
    const wchar_t * suggested;
    std::string (*fn)(const inference::Result &);
};

const FormatSpec kFmtTxt  = { L"Texto plano (*.txt)\0*.txt\0Todos\0*.*\0\0",
                              L"txt",  L"transcripcion.txt",  inference::format_txt };
const FormatSpec kFmtVtt  = { L"WebVTT (*.vtt)\0*.vtt\0Todos\0*.*\0\0",
                              L"vtt",  L"transcripcion.vtt",  inference::format_vtt };
const FormatSpec kFmtSrt  = { L"SRT (*.srt)\0*.srt\0Todos\0*.*\0\0",
                              L"srt",  L"transcripcion.srt",  inference::format_srt };
const FormatSpec kFmtJson = { L"JSON (*.json)\0*.json\0Todos\0*.*\0\0",
                              L"json", L"transcripcion.json", inference::format_json };
const FormatSpec kFmtLrc  = { L"LRC (*.lrc)\0*.lrc\0Todos\0*.*\0\0",
                              L"lrc",  L"transcripcion.lrc",  inference::format_lrc };
const FormatSpec kFmtCsv  = { L"CSV (*.csv)\0*.csv\0Todos\0*.*\0\0",
                              L"csv",  L"transcripcion.csv",  inference::format_csv };
const FormatSpec kFmtTsv  = { L"TSV (*.tsv)\0*.tsv\0Todos\0*.*\0\0",
                              L"tsv",  L"transcripcion.tsv",  inference::format_tsv };

void do_save(HWND hdlg, const FormatSpec & spec) {
    if (!g_pending) return;
    std::string content = spec.fn(*g_pending);
    prompt_and_save(hdlg, content, spec.filter, spec.ext, spec.suggested);
}

void show_more_menu(HWND hdlg, HWND button) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_SAVE_JSON, L"Guardar .json (estructura completa)");
    AppendMenuW(menu, MF_STRING, IDM_SAVE_LRC,  L"Guardar .lrc  (letra con timestamps)");
    AppendMenuW(menu, MF_STRING, IDM_SAVE_CSV,  L"Guardar .csv  (segmentos)");
    AppendMenuW(menu, MF_STRING, IDM_SAVE_TSV,  L"Guardar .tsv  (segmentos, tabulado)");

    RECT r; GetWindowRect(button, &r);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                   r.left, r.bottom, 0, hdlg, nullptr);
    DestroyMenu(menu);
}

void set_button_labels(HWND hdlg) {
    SetDlgItemTextW(hdlg, IDC_RESULT_RETRY_QUALITY, L"Retranscribir con más calidad");
    SetDlgItemTextW(hdlg, IDC_RESULT_RETRY_LANG,    L"Cambiar idioma y reintentar");
    SetDlgItemTextW(hdlg, IDC_RESULT_COPY,          L"Copiar");
    SetDlgItemTextW(hdlg, IDC_RESULT_SAVE_TXT,      L"Guardar .txt");
    SetDlgItemTextW(hdlg, IDC_RESULT_SAVE_VTT,      L"Guardar .vtt");
    SetDlgItemTextW(hdlg, IDC_RESULT_SAVE_SRT,      L"Guardar .srt");
    SetDlgItemTextW(hdlg, IDC_RESULT_SAVE_MORE,     L"Más formatos…");
    SetDlgItemTextW(hdlg, IDC_RESULT_CLOSE,         L"Cerrar");
}

INT_PTR CALLBACK ResultProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            ensure_tier_font();
            if (g_pending) {
                const inference::Result & r = *g_pending;
                SetWindowTextW(hdlg, L"Transcripción");
                SetDlgItemTextW(hdlg, IDC_RESULT_TIER, build_tier_header(r).c_str());
                if (g_tier_font) {
                    SendDlgItemMessageW(hdlg, IDC_RESULT_TIER, WM_SETFONT,
                                        (WPARAM) g_tier_font, TRUE);
                }
                SetDlgItemTextW(hdlg, IDC_RESULT_MSG,
                                inference::tier_message(r.tier, r.detected_language).c_str());
                SetDlgItemTextW(hdlg, IDC_RESULT_TEXT,  build_full_text(r).c_str());
                SetDlgItemTextW(hdlg, IDC_RESULT_INFO,  build_info_line(r).c_str());

                std::wstring worst = build_worst_segments(r);
                if (worst.empty()) {
                    SetDlgItemTextW(hdlg, IDC_RESULT_WORST_LABEL, L"");
                    ShowWindow(GetDlgItem(hdlg, IDC_RESULT_WORST), SW_HIDE);
                } else {
                    SetDlgItemTextW(hdlg, IDC_RESULT_WORST_LABEL,
                                    L"Tramos con baja confianza (considera revisar):");
                    SetDlgItemTextW(hdlg, IDC_RESULT_WORST, worst.c_str());
                }

                // Deshabilita guardar/copiar si hubo error o no hay segmentos.
                BOOL has_data = (r.error.empty() && !r.segments.empty()) ? TRUE : FALSE;
                EnableWindow(GetDlgItem(hdlg, IDC_RESULT_COPY),          has_data);
                EnableWindow(GetDlgItem(hdlg, IDC_RESULT_SAVE_TXT),      has_data);
                EnableWindow(GetDlgItem(hdlg, IDC_RESULT_SAVE_VTT),      has_data);
                EnableWindow(GetDlgItem(hdlg, IDC_RESULT_SAVE_SRT),      has_data);
                EnableWindow(GetDlgItem(hdlg, IDC_RESULT_SAVE_MORE),     has_data);
                EnableWindow(GetDlgItem(hdlg, IDC_RESULT_RETRY_QUALITY), TRUE);
                EnableWindow(GetDlgItem(hdlg, IDC_RESULT_RETRY_LANG),    TRUE);
            }
            set_button_labels(hdlg);
            SetFocus(GetDlgItem(hdlg, IDC_RESULT_CLOSE));
            return FALSE;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC) wParam;
            HWND ctl = (HWND) lParam;
            if (g_pending && ctl == GetDlgItem(hdlg, IDC_RESULT_TIER)) {
                SetTextColor(hdc, (COLORREF) inference::tier_color(g_pending->tier));
                SetBkMode(hdc, TRANSPARENT);
                return (INT_PTR) GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        }

        case WM_COMMAND: {
            const WORD id = LOWORD(wParam);
            switch (id) {
                case IDC_RESULT_COPY:
                    if (g_pending) {
                        if (copy_to_clipboard(hdlg, build_full_text(*g_pending))) {
                            // Sin notificación intrusiva; el cambio de foco basta.
                        }
                    }
                    return TRUE;

                case IDC_RESULT_SAVE_TXT: do_save(hdlg, kFmtTxt);  return TRUE;
                case IDC_RESULT_SAVE_VTT: do_save(hdlg, kFmtVtt);  return TRUE;
                case IDC_RESULT_SAVE_SRT: do_save(hdlg, kFmtSrt);  return TRUE;

                case IDC_RESULT_SAVE_MORE:
                    show_more_menu(hdlg, GetDlgItem(hdlg, IDC_RESULT_SAVE_MORE));
                    return TRUE;

                case IDM_SAVE_JSON: do_save(hdlg, kFmtJson); return TRUE;
                case IDM_SAVE_LRC:  do_save(hdlg, kFmtLrc);  return TRUE;
                case IDM_SAVE_CSV:  do_save(hdlg, kFmtCsv);  return TRUE;
                case IDM_SAVE_TSV:  do_save(hdlg, kFmtTsv);  return TRUE;

                case IDC_RESULT_RETRY_QUALITY:
                    g_outcome.action = ResultAction::RetryQuality;
                    g_outcome.new_language.clear();
                    EndDialog(hdlg, 0);
                    return TRUE;

                case IDC_RESULT_RETRY_LANG: {
                    // Pre-selecciona el idioma actualmente detectado si lo hubo,
                    // para que el usuario pueda cambiarlo cómodamente.
                    std::string initial = "auto";
                    if (g_pending && !g_pending->detected_language.empty()) {
                        int n = WideCharToMultiByte(CP_UTF8, 0,
                                    g_pending->detected_language.data(),
                                    (int) g_pending->detected_language.size(),
                                    nullptr, 0, nullptr, nullptr);
                        initial.assign((size_t) n, '\0');
                        WideCharToMultiByte(CP_UTF8, 0,
                                    g_pending->detected_language.data(),
                                    (int) g_pending->detected_language.size(),
                                    initial.data(), n, nullptr, nullptr);
                    }
                    std::string pick = pick_language(hdlg, initial);
                    if (!pick.empty()) {
                        g_outcome.action       = ResultAction::RetryWithLanguage;
                        g_outcome.new_language = pick;
                        EndDialog(hdlg, 0);
                    }
                    return TRUE;
                }

                case IDC_RESULT_CLOSE:
                case IDCANCEL:
                case IDOK:
                    g_outcome.action = ResultAction::Close;
                    EndDialog(hdlg, 0);
                    return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            g_outcome.action = ResultAction::Close;
            EndDialog(hdlg, 0);
            return TRUE;
    }
    return FALSE;
}

} // namespace

ResultOutcome show_result_dialog(HWND parent, const inference::Result & result) {
    g_pending = &result;
    g_outcome = ResultOutcome{};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_RESULT),
                    parent, ResultProc, 0);
    g_pending = nullptr;
    return g_outcome;
}

} // namespace ui
