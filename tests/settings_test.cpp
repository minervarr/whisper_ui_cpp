#include "harness.hpp"

#include "../core/settings.h"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Point XDG_CONFIG_HOME at a fresh temp dir so tests never touch the real config.
struct ConfigSandbox {
    fs::path dir;
    ConfigSandbox()
    {
        dir = fs::temp_directory_path() / ("wd_settings_test_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    }
    ~ConfigSandbox() { fs::remove_all(dir); }
};

} // namespace

TEST(config_path_shape)
{
    ConfigSandbox sb;
    std::string p = cfg::config_path();
    CHECK(!p.empty());
    std::string suffix = "whisper_destilado/config.ini";
    CHECK(p.size() > suffix.size());
    CHECK_EQ(p.substr(p.size() - suffix.size()), suffix);
    CHECK(fs::exists(fs::path(p).parent_path()));
}

TEST(missing_file_yields_defaults)
{
    ConfigSandbox sb;
    cfg::Settings s = cfg::load_settings();
    cfg::Settings d;
    CHECK_EQ(s.language, d.language);
    CHECK_EQ(s.capture_backend, d.capture_backend);
    CHECK_EQ(s.beam_size, d.beam_size);
    CHECK(s.temperature_inc == d.temperature_inc);
    CHECK_EQ(s.suppress_blank, d.suppress_blank);
}

TEST(save_load_round_trip)
{
    ConfigSandbox sb;
    cfg::Settings s;
    s.mic_device_id   = "hw:1,0";
    s.capture_backend = 2;
    s.language        = "es";
    s.translate       = true;
    s.detect_language = true;
    s.n_threads       = 6;
    s.use_beam_search = true;
    s.beam_size       = 8;
    s.best_of         = 3;
    s.temperature     = 0.4f;
    s.temperature_inc = 0.1f;
    s.length_penalty  = 0.5f;
    s.entropy_thold   = 2.7f;
    s.logprob_thold   = -0.5f;
    s.no_speech_thold = 0.35f;
    s.n_max_text_ctx  = 2048;
    s.max_len         = 60;
    s.max_tokens      = 32;
    s.audio_ctx       = 512;
    s.initial_prompt  = "hola, con acentos áéíóú";
    s.carry_initial_prompt = true;
    s.suppress_blank  = false;
    s.suppress_nst    = false;
    s.single_segment  = true;
    s.split_on_word   = true;
    s.tdrz_enable     = true;
    s.print_progress  = true;
    s.print_realtime  = true;

    CHECK(cfg::save_settings(s));
    cfg::Settings r = cfg::load_settings();

    CHECK_EQ(r.mic_device_id,   s.mic_device_id);
    CHECK_EQ(r.capture_backend, s.capture_backend);
    CHECK_EQ(r.language,        s.language);
    CHECK_EQ(r.translate,       s.translate);
    CHECK_EQ(r.detect_language, s.detect_language);
    CHECK_EQ(r.n_threads,       s.n_threads);
    CHECK_EQ(r.use_beam_search, s.use_beam_search);
    CHECK_EQ(r.beam_size,       s.beam_size);
    CHECK_EQ(r.best_of,         s.best_of);
    CHECK(r.temperature     == s.temperature);
    CHECK(r.temperature_inc == s.temperature_inc);
    CHECK(r.length_penalty  == s.length_penalty);
    CHECK(r.entropy_thold   == s.entropy_thold);
    CHECK(r.logprob_thold   == s.logprob_thold);
    CHECK(r.no_speech_thold == s.no_speech_thold);
    CHECK_EQ(r.n_max_text_ctx, s.n_max_text_ctx);
    CHECK_EQ(r.max_len,        s.max_len);
    CHECK_EQ(r.max_tokens,     s.max_tokens);
    CHECK_EQ(r.audio_ctx,      s.audio_ctx);
    CHECK_EQ(r.initial_prompt, s.initial_prompt);
    CHECK_EQ(r.carry_initial_prompt, s.carry_initial_prompt);
    CHECK_EQ(r.suppress_blank, s.suppress_blank);
    CHECK_EQ(r.suppress_nst,   s.suppress_nst);
    CHECK_EQ(r.single_segment, s.single_segment);
    CHECK_EQ(r.split_on_word,  s.split_on_word);
    CHECK_EQ(r.tdrz_enable,    s.tdrz_enable);
    CHECK_EQ(r.print_progress, s.print_progress);
    CHECK_EQ(r.print_realtime, s.print_realtime);
}

TEST(partial_file_keeps_defaults)
{
    ConfigSandbox sb;
    std::string path = cfg::config_path();
    std::ofstream f(path, std::ios::binary);
    f << "# hand-written partial config\n";
    f << "[whisper]\n";
    f << "language = fr\n";           // whitespace around '=' must be tolerated
    f << "beam_size=9\n";
    f << "garbage line without equals\n";
    f.close();

    cfg::Settings s = cfg::load_settings();
    cfg::Settings d;
    CHECK_EQ(s.language, std::string("fr"));
    CHECK_EQ(s.beam_size, 9);
    CHECK_EQ(s.best_of, d.best_of);
    CHECK(s.temperature_inc == d.temperature_inc);
    CHECK_EQ(s.mic_device_id, d.mic_device_id);
}

TEST(quality_preset)
{
    cfg::Settings s;
    s.beam_size = 2;
    s.best_of = 1;
    cfg::Settings q = s.with_quality_preset();
    CHECK(q.use_beam_search);
    CHECK_EQ(q.beam_size, 5);
    CHECK_EQ(q.best_of, 5);
    CHECK(q.temperature_inc == s.temperature_inc);
}
