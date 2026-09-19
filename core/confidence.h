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

// Confidence for ONE segment read from whisper's current result_all at index
// `index`. Indexes are only valid right after a whisper_full call, while that
// call's segments still live in the context — grouped transcriptions (see
// transcribe.cpp build_call_units) call this once per group/segment and then
// aggregate_confidence() over the merged result.
void compute_segment_confidence(whisper_context * ctx, int index, Segment & seg);

// Recomputes Result::{confidence_overall, tier, worst_segments} from the
// already-filled per-segment confidence fields. Needs no whisper context, so
// it works on segments merged from several whisper_full calls.
void aggregate_confidence(Result & r);

// Actionable Spanish message for the tier, with the detected language code.
std::string tier_message(ConfidenceTier tier, const std::string & detected_language);

// Short tier label ("Excelente", "Buena", "Baja").
const char * tier_label(ConfidenceTier tier);

// Highlight color for the tier (rgb() packing above).
uint32_t tier_color(ConfidenceTier tier);

} // namespace inference
