#include "core/confidence.h"

#include <algorithm>
#include <vector>

#include "whisper.h"

namespace inference {

namespace {

constexpr float  kLowTokenThreshold  = 0.30f;
constexpr size_t kMaxWorstSegments   = 3;
constexpr float  kTierExcellent      = 0.85f;
constexpr float  kTierGood           = 0.65f;

ConfidenceTier classify(float c) {
    if (c >= kTierExcellent) return ConfidenceTier::Excellent;
    if (c >= kTierGood)      return ConfidenceTier::Good;
    return ConfidenceTier::Low;
}

} // namespace

void compute_confidence(whisper_context * ctx, Result & r) {
    if (!ctx || r.segments.empty()) {
        r.confidence_overall = 0.0f;
        r.tier               = ConfidenceTier::Low;
        return;
    }

    // Tokens with id >= tok_eot are specials or timestamps in whisper's vocab.
    const whisper_token tok_eot = whisper_token_eot(ctx);

    double sum_weighted  = 0.0;
    double sum_durations = 0.0;

    for (size_t i = 0; i < r.segments.size(); ++i) {
        Segment & seg = r.segments[i];

        const int n_tokens = whisper_full_n_tokens(ctx, (int) i);
        double sum_p   = 0.0;
        int    n_valid = 0;
        float  min_p   = 1.0f;

        for (int t = 0; t < n_tokens; ++t) {
            whisper_token_data td = whisper_full_get_token_data(ctx, (int) i, t);
            if (td.id >= tok_eot) continue;  // skip timestamps and specials
            sum_p += td.p;
            ++n_valid;
            if (td.p < min_p) min_p = td.p;
        }

        seg.no_speech_prob = whisper_full_get_segment_no_speech_prob(ctx, (int) i);
        seg.mean_token_p   = (n_valid > 0) ? float(sum_p / n_valid) : 0.0f;
        seg.min_token_p    = (n_valid > 0) ? min_p : 0.0f;
        seg.confidence     = seg.mean_token_p * (1.0f - seg.no_speech_prob);
        if (seg.confidence < 0.0f) seg.confidence = 0.0f;

        const double dur = std::max<double>(1.0, double(seg.t1_ms - seg.t0_ms));
        sum_weighted  += double(seg.confidence) * dur;
        sum_durations += dur;
    }

    r.confidence_overall = (sum_durations > 0.0) ? float(sum_weighted / sum_durations) : 0.0f;
    r.tier               = classify(r.confidence_overall);

    // Top-K worst segments by min_token_p, only those below the threshold.
    std::vector<size_t> indices(r.segments.size());
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        return r.segments[a].min_token_p < r.segments[b].min_token_p;
    });
    r.worst_segments.clear();
    for (size_t k = 0; k < indices.size() && r.worst_segments.size() < kMaxWorstSegments; ++k) {
        if (r.segments[indices[k]].min_token_p < kLowTokenThreshold) {
            r.worst_segments.push_back(indices[k]);
        }
    }
}

const char * tier_label(ConfidenceTier t) {
    switch (t) {
        case ConfidenceTier::Excellent: return "Excelente";
        case ConfidenceTier::Good:      return "Buena";
        case ConfidenceTier::Low:       return "Baja";
    }
    return "-";
}

uint32_t tier_color(ConfidenceTier t) {
    switch (t) {
        case ConfidenceTier::Excellent: return rgb( 30, 150,  30);
        case ConfidenceTier::Good:      return rgb(200, 140,   0);
        case ConfidenceTier::Low:       return rgb(200,  40,  40);
    }
    return rgb(0, 0, 0);
}

std::string tier_message(ConfidenceTier t, const std::string & detected_language) {
    switch (t) {
        case ConfidenceTier::Excellent:
            return "La transcripción tiene alta confianza token a token. "
                   "Probablemente es fiel al audio.";
        case ConfidenceTier::Good:
            return "Confianza aceptable. Si dudas de algún tramo, puedes retranscribir "
                   "con \"Retranscribir con más calidad\" (beam search) — usa la misma "
                   "grabación, no necesitas volver a hablar.";
        case ConfidenceTier::Low: {
            std::string m =
                "Confianza baja. Causas posibles: idioma incorrecto detectado";
            if (!detected_language.empty()) {
                m += " (whisper detectó '" + detected_language + "')";
            }
            m += ", ruido de fondo, audio recortado o micrófono distorsionado. "
                 "Sugerencias: fuerza manualmente el idioma desde el desplegable "
                 "y vuelve a transcribir, o revisa los tramos marcados abajo.";
            return m;
        }
    }
    return "";
}

} // namespace inference
