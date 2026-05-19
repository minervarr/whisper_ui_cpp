#pragma once

#include <string>

namespace cfg {

// Refleja todos los campos relevantes de whisper_full_params + algunos de control.
// El nivel "rápido" corresponde a defaults razonables; el diálogo Avanzado
// permite tocar todo.
struct Settings {
    // --- Dispositivo de captura ---
    // Endpoint ID estable de IMMDevice. "" = "usar el default del sistema".
    std::wstring mic_device_id;

    // --- Idioma / traducción ---
    std::string language        = "auto";
    bool        translate       = false;
    bool        detect_language = false;

    // --- Hilos ---
    int         n_threads = 0;   // 0 => auto (min(8, hardware_concurrency))

    // --- Estrategia de muestreo ---
    bool        use_beam_search = false;
    int         beam_size       = 5;
    int         best_of         = 5;

    // --- Tunables del decoder ---
    float       temperature      = 0.0f;
    float       temperature_inc  = 0.2f;   // escalera de fallback — NO desactivar a la ligera
    float       length_penalty   = -1.0f;
    float       entropy_thold    = 2.4f;
    float       logprob_thold    = -1.0f;
    float       no_speech_thold  = 0.6f;

    // --- Límites de tokens / contexto ---
    int         n_max_text_ctx = 16384;
    int         max_len        = 0;        // 0 = sin límite por segmento
    int         max_tokens     = 0;        // 0 = sin límite
    int         audio_ctx      = 0;        // 0 = default del modelo

    // --- Prompt ---
    std::string initial_prompt;
    bool        carry_initial_prompt = false;

    // --- Comportamiento ---
    bool        suppress_blank   = true;
    bool        suppress_nst     = true;
    bool        single_segment   = false;
    bool        split_on_word    = false;
    bool        tdrz_enable      = false;

    // --- Debug ---
    bool        print_progress   = false;
    bool        print_realtime   = false;

    // Devuelve una copia con preset "Quality" (beam search + best_of) — usado por Fase 9.
    Settings with_quality_preset() const;

    // Devuelve los defaults del modo rápido.
    static Settings fast_defaults();
};

// Ruta absoluta del INI de configuración (creando el directorio padre si hace falta).
std::wstring config_path();

// Carga el INI (o aplica defaults si no existe / está parcial).
Settings load_settings();

// Guarda el INI (sobrescribe).
bool save_settings(const Settings & s);

} // namespace cfg
