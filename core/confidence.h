#pragma once

#include <cstdint>
#include <string>

#include "core/transcribe.h"

struct whisper_context;

namespace inference {

// Theme-neutral color packing: 0x00BBGGRR (same layout the old COLORREF used,
// so the historical values carry over untouched).
constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return r | (g << 8) | (b << 16);
}

// Computes per-segment confidence, duration-weighted overall confidence and
// tier. Fills Segment::{no_speech_prob, mean_token_p, min_token_p, confidence}
// and Result::{confidence_overall, tier, worst_segments}.
void compute_confidence(whisper_context * ctx, Result & r);

// Actionable Spanish message for the tier, with the detected language code.
std::string tier_message(ConfidenceTier tier, const std::string & detected_language);

// Short tier label ("Excelente", "Buena", "Baja").
const char * tier_label(ConfidenceTier tier);

// Highlight color for the tier (rgb() packing above).
uint32_t tier_color(ConfidenceTier tier);

} // namespace inference
