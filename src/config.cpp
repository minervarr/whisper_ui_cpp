#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>

namespace cfg {

namespace {

constexpr const wchar_t * kSection = L"whisper";

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

std::wstring appdata_dir() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
        return {};
    }
    std::wstring out = p;
    CoTaskMemFree(p);
    out += L"\\whisper_destilado";
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    return out;
}

int get_int(const wchar_t * key, int def, const wchar_t * path) {
    return (int) GetPrivateProfileIntW(kSection, key, def, path);
}

float get_float(const wchar_t * key, float def, const wchar_t * path) {
    wchar_t buf[64];
    wchar_t defbuf[64];
    swprintf(defbuf, 64, L"%g", def);
    GetPrivateProfileStringW(kSection, key, defbuf, buf, 64, path);
    return (float) _wtof(buf);
}

std::wstring get_string(const wchar_t * key, const wchar_t * def, const wchar_t * path) {
    wchar_t buf[4096];
    GetPrivateProfileStringW(kSection, key, def, buf, 4096, path);
    return buf;
}

bool get_bool(const wchar_t * key, bool def, const wchar_t * path) {
    return get_int(key, def ? 1 : 0, path) != 0;
}

void put_int(const wchar_t * key, int v, const wchar_t * path) {
    wchar_t b[32]; swprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(kSection, key, b, path);
}

void put_float(const wchar_t * key, float v, const wchar_t * path) {
    wchar_t b[64]; swprintf(b, 64, L"%g", v);
    WritePrivateProfileStringW(kSection, key, b, path);
}

void put_bool(const wchar_t * key, bool v, const wchar_t * path) {
    WritePrivateProfileStringW(kSection, key, v ? L"1" : L"0", path);
}

void put_string(const wchar_t * key, const std::wstring & v, const wchar_t * path) {
    WritePrivateProfileStringW(kSection, key, v.c_str(), path);
}

} // namespace

Settings Settings::fast_defaults() {
    return Settings{};   // los defaults del struct ya son los del modo rápido
}

Settings Settings::with_quality_preset() const {
    Settings q = *this;
    q.use_beam_search = true;
    if (q.beam_size < 5) q.beam_size = 5;
    if (q.best_of   < 5) q.best_of   = 5;
    // temperature_inc se queda en su valor — la escalera de fallback ayuda a la calidad.
    return q;
}

std::wstring config_path() {
    auto d = appdata_dir();
    if (d.empty()) return L"";
    return d + L"\\config.ini";
}

Settings load_settings() {
    Settings s;
    std::wstring path = config_path();
    if (path.empty()) return s;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return s;

    s.mic_device_id   = get_string(L"mic_device_id", L"", path.c_str());

    s.language        = wide_to_utf8(get_string(L"language", L"auto", path.c_str()));
    s.translate       = get_bool  (L"translate",       s.translate,       path.c_str());
    s.detect_language = get_bool  (L"detect_language", s.detect_language, path.c_str());
    s.n_threads       = get_int   (L"n_threads",       s.n_threads,       path.c_str());

    s.use_beam_search = get_bool  (L"use_beam_search", s.use_beam_search, path.c_str());
    s.beam_size       = get_int   (L"beam_size",       s.beam_size,       path.c_str());
    s.best_of         = get_int   (L"best_of",         s.best_of,         path.c_str());

    s.temperature     = get_float (L"temperature",     s.temperature,     path.c_str());
    s.temperature_inc = get_float (L"temperature_inc", s.temperature_inc, path.c_str());
    s.length_penalty  = get_float (L"length_penalty",  s.length_penalty,  path.c_str());
    s.entropy_thold   = get_float (L"entropy_thold",   s.entropy_thold,   path.c_str());
    s.logprob_thold   = get_float (L"logprob_thold",   s.logprob_thold,   path.c_str());
    s.no_speech_thold = get_float (L"no_speech_thold", s.no_speech_thold, path.c_str());

    s.n_max_text_ctx  = get_int   (L"n_max_text_ctx",  s.n_max_text_ctx,  path.c_str());
    s.max_len         = get_int   (L"max_len",         s.max_len,         path.c_str());
    s.max_tokens      = get_int   (L"max_tokens",      s.max_tokens,      path.c_str());
    s.audio_ctx       = get_int   (L"audio_ctx",       s.audio_ctx,       path.c_str());

    s.initial_prompt        = wide_to_utf8(get_string(L"initial_prompt", L"", path.c_str()));
    s.carry_initial_prompt  = get_bool(L"carry_initial_prompt", s.carry_initial_prompt, path.c_str());

    s.suppress_blank  = get_bool(L"suppress_blank",  s.suppress_blank,  path.c_str());
    s.suppress_nst    = get_bool(L"suppress_nst",    s.suppress_nst,    path.c_str());
    s.single_segment  = get_bool(L"single_segment",  s.single_segment,  path.c_str());
    s.split_on_word   = get_bool(L"split_on_word",   s.split_on_word,   path.c_str());
    s.tdrz_enable     = get_bool(L"tdrz_enable",     s.tdrz_enable,     path.c_str());
    s.print_progress  = get_bool(L"print_progress",  s.print_progress,  path.c_str());
    s.print_realtime  = get_bool(L"print_realtime",  s.print_realtime,  path.c_str());

    return s;
}

bool save_settings(const Settings & s) {
    std::wstring path = config_path();
    if (path.empty()) return false;

    put_string(L"mic_device_id",   s.mic_device_id,                 path.c_str());
    put_string(L"language",        utf8_to_wide(s.language),        path.c_str());
    put_bool  (L"translate",       s.translate,       path.c_str());
    put_bool  (L"detect_language", s.detect_language, path.c_str());
    put_int   (L"n_threads",       s.n_threads,       path.c_str());

    put_bool  (L"use_beam_search", s.use_beam_search, path.c_str());
    put_int   (L"beam_size",       s.beam_size,       path.c_str());
    put_int   (L"best_of",         s.best_of,         path.c_str());

    put_float (L"temperature",     s.temperature,     path.c_str());
    put_float (L"temperature_inc", s.temperature_inc, path.c_str());
    put_float (L"length_penalty",  s.length_penalty,  path.c_str());
    put_float (L"entropy_thold",   s.entropy_thold,   path.c_str());
    put_float (L"logprob_thold",   s.logprob_thold,   path.c_str());
    put_float (L"no_speech_thold", s.no_speech_thold, path.c_str());

    put_int   (L"n_max_text_ctx",  s.n_max_text_ctx,  path.c_str());
    put_int   (L"max_len",         s.max_len,         path.c_str());
    put_int   (L"max_tokens",      s.max_tokens,      path.c_str());
    put_int   (L"audio_ctx",       s.audio_ctx,       path.c_str());

    put_string(L"initial_prompt", utf8_to_wide(s.initial_prompt), path.c_str());
    put_bool  (L"carry_initial_prompt", s.carry_initial_prompt, path.c_str());

    put_bool  (L"suppress_blank",  s.suppress_blank,  path.c_str());
    put_bool  (L"suppress_nst",    s.suppress_nst,    path.c_str());
    put_bool  (L"single_segment",  s.single_segment,  path.c_str());
    put_bool  (L"split_on_word",   s.split_on_word,   path.c_str());
    put_bool  (L"tdrz_enable",     s.tdrz_enable,     path.c_str());
    put_bool  (L"print_progress",  s.print_progress,  path.c_str());
    put_bool  (L"print_realtime",  s.print_realtime,  path.c_str());

    return true;
}

} // namespace cfg
