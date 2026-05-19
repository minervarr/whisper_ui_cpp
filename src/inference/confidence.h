#pragma once

#include "transcribe.h"

struct whisper_context;

namespace inference {

// Calcula confianza por segmento, confianza global ponderada y tier.
// Llena Segment::{no_speech_prob, mean_token_p, min_token_p, confidence}
// y Result::{confidence_overall, tier, worst_segments}.
void compute_confidence(whisper_context * ctx, Result & r);

// Mensaje accionable en español para el tier dado, con el código de idioma detectado.
std::wstring tier_message(ConfidenceTier tier, const std::wstring & detected_language);

// Etiqueta corta del tier ("Excelente", "Buena", "Baja").
const wchar_t * tier_label(ConfidenceTier tier);

// Color de texto (RGB) para resaltar el tier.
unsigned long tier_color(ConfidenceTier tier);

} // namespace inference
