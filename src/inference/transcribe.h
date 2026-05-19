#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config.h"

struct whisper_context;

namespace inference {

// Mensaje posteado a la ventana principal cuando termina la transcripción.
// wParam: 0 = OK, 1 = error (texto del error en el campo `error` del Result).
// lParam: const Result *  (heap-alojado; el receptor debe `delete`).
constexpr UINT WM_TRANSCRIBE_DONE = WM_APP + 1;

struct Segment {
    int64_t      t0_ms = 0;
    int64_t      t1_ms = 0;
    std::wstring text;

    // Métricas de confianza (calculadas en confidence.cpp).
    float        no_speech_prob = 0.0f;
    float        mean_token_p   = 0.0f;
    float        min_token_p    = 0.0f;
    float        confidence     = 0.0f; // mean_token_p * (1 - no_speech_prob)
};

enum class ConfidenceTier {
    Excellent,
    Good,
    Low,
};

struct Result {
    std::vector<Segment> segments;
    int64_t              total_duration_ms = 0;
    std::wstring         detected_language;
    std::wstring         error;

    float                confidence_overall = 0.0f;
    ConfidenceTier       tier = ConfidenceTier::Excellent;
    std::vector<size_t>  worst_segments;
};

// Lanza un hilo detached que invoca whisper_full sobre el buffer dado y postea
// WM_TRANSCRIBE_DONE a `hwnd_notify` con el Result *.
// `settings` se copia internamente (los strings duran tanto como el hilo).
void transcribe_async(whisper_context * ctx,
                      std::shared_ptr<std::vector<float>> samples,
                      const cfg::Settings & settings,
                      HWND hwnd_notify);

// Espera a que terminen todos los workers de transcripción en vuelo.
// Pensado para llamarse en WM_DESTROY antes de liberar whisper_context.
void join_pending_workers();

} // namespace inference
