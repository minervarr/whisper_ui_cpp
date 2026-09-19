#pragma once

#include <string>

namespace cfg {

// Mirrors the relevant whisper_full_params fields plus app-level control.
// The "fast" level is the struct defaults; the advanced settings view can
// touch everything.
struct Settings {
    // --- Capture device ---
    // Backend-specific stable id ("hw:1,0" | "LIBUSB:vid:pid" | WASAPI
    // endpoint id | "" for JACK/server default). Empty = system default.
    std::string mic_device_id;
    // BackendKind as int (core/audio/capture.h): 0 Wasapi, 1 Alsa, 2 Jack, 3 Usb.
    int         capture_backend = -1;   // -1 = not chosen yet (auto-pick)

    // --- GPU ---
    // whisper gpu_device index (GPU/iGPU in ggml order, see core/gpu_devices.h).
    // -1 = auto: prefer the first discrete GPU, else the first integrated one.
    int         gpu_device = -1;

    // --- Language / translation ---
    std::string language        = "auto";
    bool        translate       = false;
    bool        detect_language = false;

    // --- Threads ---
    int         n_threads = 0;   // 0 => auto (min(8, hardware_concurrency))

    // --- Sampling strategy ---
    bool        use_beam_search = false;
    int         beam_size       = 5;
    int         best_of         = 5;

    // --- Decoder tunables ---
    float       temperature      = 0.0f;
    float       temperature_inc  = 0.2f;   // fallback ladder — do not disable lightly
    float       length_penalty   = -1.0f;
    float       entropy_thold    = 2.4f;
    float       logprob_thold    = -1.0f;
    float       no_speech_thold  = 0.6f;

    // --- Token / context limits ---
    int         n_max_text_ctx = 16384;
    int         max_len        = 0;        // 0 = no per-segment limit
    int         max_tokens     = 0;        // 0 = no limit
    int         audio_ctx      = 0;        // 0 = model default

    // --- Prompt ---
    std::string initial_prompt;
    bool        carry_initial_prompt = false;

    // --- Behavior ---
    bool        suppress_blank   = true;
    bool        suppress_nst     = true;
    bool        single_segment   = false;
    bool        split_on_word    = false;
    bool        tdrz_enable      = false;

    // --- Anti-hallucination guard ---
    // Cuts long silent gaps out of the audio before whisper sees them (its #1
    // trigger for repetition loops over long files); timestamps are mapped
    // back to the absolute timeline afterwards. On by default.
    bool        silence_trim     = true;

    // Capture per-token detail (text, id, probabilities, timestamps) into the
    // Result. Off by default so the GUI path stays light; the CLI's --full /
    // --output json-full enables it via with_max_info_preset().
    bool        keep_tokens      = false;

    // --- Debug ---
    bool        print_progress   = false;
    bool        print_realtime   = false;

    // Copy with the "Quality" preset applied (beam search + best_of).
    Settings with_quality_preset() const;

    // Copy with the "most information" preset: keep non-speech/special tokens
    // (silence, music...), keep blanks, split words, and capture token-level
    // detail — the transcription loses as little of the audio as whisper can.
    Settings with_max_info_preset() const;

    // Fast-mode defaults.
    static Settings fast_defaults();
};

// Absolute path of the config INI (creates the parent directory if needed).
// Linux: $XDG_CONFIG_HOME/whisper_destilado/config.ini (or ~/.config/...).
// Windows: %APPDATA%\whisper_destilado\config.ini.
std::string config_path();

// Loads the INI (defaults if missing; absent keys keep their defaults).
Settings load_settings();

// Saves the INI (overwrites).
bool save_settings(const Settings & s);

} // namespace cfg
