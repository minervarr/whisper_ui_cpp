#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/settings.h"
#include "core/event_queue.h"

struct whisper_context;

namespace inference {

// One output token of a segment, captured only when cfg::Settings::keep_tokens
// is set (the CLI's max-info mode). Times are milliseconds.
struct TokenInfo {
    std::string text;         // raw token text (UTF-8, may start with a space)
    int         id = 0;       // vocabulary id
    float       p = 0.0f;     // probability of the token
    float       plog = 0.0f;  // log probability
    int64_t     t0_ms = 0;    // start time
    int64_t     t1_ms = 0;    // end time
    float       vlen = 0.0f;  // voice length of the token (acoustic detail)
    bool        is_special = false;  // id >= tok_eot: timestamp/special token
};

struct Segment {
    int64_t     t0_ms = 0;
    int64_t     t1_ms = 0;
    std::string text;                    // UTF-8

    // Confidence metrics (computed in confidence.cpp).
    float       no_speech_prob = 0.0f;
    float       mean_token_p   = 0.0f;
    float       min_token_p    = 0.0f;
    float       confidence     = 0.0f;   // mean_token_p * (1 - no_speech_prob)

    // Token-level detail (empty unless keep_tokens was set).
    std::vector<TokenInfo> tokens;

    // True when this segment is a duplicate inside a consecutive repetition
    // run (hallucination-loop guard). Human-readable outputs collapse these;
    // json/json-full keep every segment and expose the flag. The run's first
    // occurrence(s) stay clean.
    bool        rep_run = false;
};

enum class ConfidenceTier {
    Excellent,
    Good,
    Low,
};

struct Result {
    std::vector<Segment> segments;
    int64_t              total_duration_ms = 0;
    std::string          detected_language;   // UTF-8 code, e.g. "es"
    std::string          error;               // UTF-8, Spanish, empty = OK

    float                confidence_overall = 0.0f;
    ConfidenceTier       tier = ConfidenceTier::Excellent;
    std::vector<size_t>  worst_segments;

    // Snapshot of the settings that produced this result (exported by
    // format_json_full so the transcription is self-describing), plus the
    // whisper_full wall time in ms.
    cfg::Settings        settings_used;
    int64_t              transcribe_ms = 0;
};

// ── Anti-hallucination helpers (testable without a whisper context) ─────────

// Speech interval in input sample indexes (16 kHz mono float buffer).
struct SpeechSpan {
    int64_t begin = 0;
    int64_t end   = 0;
};

// Energy-based speech detection: 32 ms RMS windows, `threshold` (linear
// amplitude) counts as speech; a ~300 ms pad is kept around each edge so the
// model never starts/ends mid-word; gaps shorter than `min_silence_ms` are
// bridged and blips shorter than `min_speech_ms` are dropped. Empty result =
// no speech at all (caller must fall back to the unmodified buffer).
std::vector<SpeechSpan> detect_speech_spans(const std::vector<float> & samples,
                                            float threshold = 0.004f,
                                            int min_silence_ms = 2000,
                                            int min_speech_ms = 250);

// Concatenates the kept spans into `out_clipped` (original order preserved)
// and returns the monotonic map from clipped-domain time back to absolute
// input time (input ms -> input samples are both at 16 kHz).
struct ClipMap {
    std::vector<SpeechSpan> kept;         // kept spans in ORIGINAL sample coords
    std::vector<int64_t>    clipped_end;  // cumulative clipped sample count after each span
    int64_t map_ms(int64_t clipped_ms) const;  // clipped ms -> absolute input ms
};
ClipMap build_clip_map(const std::vector<float> & samples,
                       const std::vector<SpeechSpan> & spans,
                       std::vector<float> * out_clipped);

// A slice of audio handed to a single whisper_full call, in sample coords of
// the same buffer the call receives (the clipped buffer, or the original one).
struct CallUnit {
    int64_t clip_offset = 0;  // sample offset into the buffer
    int64_t clip_len    = 0;  // sample count
};

// Partitions consecutive speech spans into whisper_full call units holding at
// most `max_group_samples` of audio each. Long uninterrupted spans are sliced
// into several units so no single decode has to survive minutes of continuous
// speech (whisper's degeneration point on this app's long recordings). The
// units tile the kept audio exactly, in order. Empty input -> empty output.
std::vector<CallUnit> build_call_units(const std::vector<SpeechSpan> & spans,
                                       int64_t max_group_samples);

// Marks every duplicate member of a consecutive repetition run with rep_run:
// >= 3 consecutive identical texts (period 1) or >= 6 segments alternating
// between the same two texts (period 2 — the "Y… / si…" loop shape). The
// run's first full period stays clean. Conservative on purpose: real speech
// almost never repeats exact text that often; json-full keeps everything.
void mark_repetition_runs(std::vector<Segment> & segments);

// Launches a detached worker that runs whisper_full over the buffer and pushes
// a TranscribeDone AppEvent (result heap-allocated; receiver deletes) into
// `queue`. `settings` is copied internally so its strings outlive the thread.
void transcribe_async(whisper_context * ctx,
                      std::shared_ptr<std::vector<float>> samples,
                      const cfg::Settings & settings,
                      core::EventQueue & queue);

// Waits for all in-flight transcription workers. Call before freeing the
// whisper_context at shutdown.
void join_pending_workers();

} // namespace inference
