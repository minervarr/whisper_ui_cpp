#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace cfg {

namespace {

std::string config_dir()
{
#ifdef _WIN32
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)))
        return {};
    // UTF-16 -> UTF-8
    int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
    std::string base(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, p, -1, base.data(), n, nullptr, nullptr);
    CoTaskMemFree(p);
    if (base.empty()) return {};
    return base + "\\whisper_destilado";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) return {};
        base = std::string(home) + "/.config";
    }
    return base + "/whisper_destilado";
#endif
}

// --- Minimal INI: "key=value" lines, optional "[section]" lines (ignored),
//     '#' or ';' full-line comments, surrounding whitespace trimmed. ---

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::map<std::string, std::string> parse_ini(const std::string& path)
{
    std::map<std::string, std::string> kv;
    std::ifstream in(path, std::ios::binary);
    if (!in) return kv;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return kv;
}

using Map = std::map<std::string, std::string>;

std::string get_string(const Map& kv, const char* key, const std::string& def)
{
    auto it = kv.find(key);
    return it == kv.end() ? def : it->second;
}

int get_int(const Map& kv, const char* key, int def)
{
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

float get_float(const Map& kv, const char* key, float def)
{
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

bool get_bool(const Map& kv, const char* key, bool def)
{
    return get_int(kv, key, def ? 1 : 0) != 0;
}

std::string fmt_float(float v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", v);   // round-trips binary32 exactly
    return buf;
}

} // namespace

Settings Settings::fast_defaults()
{
    return Settings{};   // struct defaults ARE the fast mode
}

Settings Settings::with_quality_preset() const
{
    Settings q = *this;
    q.use_beam_search = true;
    if (q.beam_size < 5) q.beam_size = 5;
    if (q.best_of   < 5) q.best_of   = 5;
    // temperature_inc stays — the fallback ladder helps quality.
    return q;
}

std::string config_path()
{
    std::string d = config_dir();
    if (d.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
#ifdef _WIN32
    return d + "\\config.ini";
#else
    return d + "/config.ini";
#endif
}

Settings load_settings()
{
    Settings s;
    std::string path = config_path();
    if (path.empty()) return s;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return s;

    Map kv = parse_ini(path);

    s.mic_device_id   = get_string(kv, "mic_device_id",   s.mic_device_id);
    s.capture_backend = get_int   (kv, "capture_backend", s.capture_backend);

    s.language        = get_string(kv, "language",        s.language);
    s.translate       = get_bool  (kv, "translate",       s.translate);
    s.detect_language = get_bool  (kv, "detect_language", s.detect_language);
    s.n_threads       = get_int   (kv, "n_threads",       s.n_threads);

    s.use_beam_search = get_bool  (kv, "use_beam_search", s.use_beam_search);
    s.beam_size       = get_int   (kv, "beam_size",       s.beam_size);
    s.best_of         = get_int   (kv, "best_of",         s.best_of);

    s.temperature     = get_float (kv, "temperature",     s.temperature);
    s.temperature_inc = get_float (kv, "temperature_inc", s.temperature_inc);
    s.length_penalty  = get_float (kv, "length_penalty",  s.length_penalty);
    s.entropy_thold   = get_float (kv, "entropy_thold",   s.entropy_thold);
    s.logprob_thold   = get_float (kv, "logprob_thold",   s.logprob_thold);
    s.no_speech_thold = get_float (kv, "no_speech_thold", s.no_speech_thold);

    s.n_max_text_ctx  = get_int   (kv, "n_max_text_ctx",  s.n_max_text_ctx);
    s.max_len         = get_int   (kv, "max_len",         s.max_len);
    s.max_tokens      = get_int   (kv, "max_tokens",      s.max_tokens);
    s.audio_ctx       = get_int   (kv, "audio_ctx",       s.audio_ctx);

    s.initial_prompt       = get_string(kv, "initial_prompt", s.initial_prompt);
    s.carry_initial_prompt = get_bool  (kv, "carry_initial_prompt", s.carry_initial_prompt);

    s.suppress_blank  = get_bool(kv, "suppress_blank",  s.suppress_blank);
    s.suppress_nst    = get_bool(kv, "suppress_nst",    s.suppress_nst);
    s.single_segment  = get_bool(kv, "single_segment",  s.single_segment);
    s.split_on_word   = get_bool(kv, "split_on_word",   s.split_on_word);
    s.tdrz_enable     = get_bool(kv, "tdrz_enable",     s.tdrz_enable);
    s.print_progress  = get_bool(kv, "print_progress",  s.print_progress);
    s.print_realtime  = get_bool(kv, "print_realtime",  s.print_realtime);

    return s;
}

bool save_settings(const Settings & s)
{
    std::string path = config_path();
    if (path.empty()) return false;

    std::ostringstream out;
    out << "[whisper]\n";
    out << "mic_device_id="   << s.mic_device_id            << "\n";
    out << "capture_backend=" << s.capture_backend          << "\n";
    out << "language="        << s.language                 << "\n";
    out << "translate="       << (s.translate ? 1 : 0)      << "\n";
    out << "detect_language=" << (s.detect_language ? 1 : 0)<< "\n";
    out << "n_threads="       << s.n_threads                << "\n";
    out << "use_beam_search=" << (s.use_beam_search ? 1 : 0)<< "\n";
    out << "beam_size="       << s.beam_size                << "\n";
    out << "best_of="         << s.best_of                  << "\n";
    out << "temperature="     << fmt_float(s.temperature)     << "\n";
    out << "temperature_inc=" << fmt_float(s.temperature_inc) << "\n";
    out << "length_penalty="  << fmt_float(s.length_penalty)  << "\n";
    out << "entropy_thold="   << fmt_float(s.entropy_thold)   << "\n";
    out << "logprob_thold="   << fmt_float(s.logprob_thold)   << "\n";
    out << "no_speech_thold=" << fmt_float(s.no_speech_thold) << "\n";
    out << "n_max_text_ctx="  << s.n_max_text_ctx           << "\n";
    out << "max_len="         << s.max_len                  << "\n";
    out << "max_tokens="      << s.max_tokens               << "\n";
    out << "audio_ctx="       << s.audio_ctx                << "\n";
    out << "initial_prompt="  << s.initial_prompt           << "\n";
    out << "carry_initial_prompt=" << (s.carry_initial_prompt ? 1 : 0) << "\n";
    out << "suppress_blank="  << (s.suppress_blank ? 1 : 0) << "\n";
    out << "suppress_nst="    << (s.suppress_nst ? 1 : 0)   << "\n";
    out << "single_segment="  << (s.single_segment ? 1 : 0) << "\n";
    out << "split_on_word="   << (s.split_on_word ? 1 : 0)  << "\n";
    out << "tdrz_enable="     << (s.tdrz_enable ? 1 : 0)    << "\n";
    out << "print_progress="  << (s.print_progress ? 1 : 0) << "\n";
    out << "print_realtime="  << (s.print_realtime ? 1 : 0) << "\n";

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << out.str();
    return (bool) f;
}

} // namespace cfg
