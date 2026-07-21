#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/settings.h"
#include "core/event_queue.h"

struct whisper_context;

namespace inference {

struct Segment {
    int64_t     t0_ms = 0;
    int64_t     t1_ms = 0;
    std::string text;                    // UTF-8

    // Confidence metrics (computed in confidence.cpp).
    float       no_speech_prob = 0.0f;
    float       mean_token_p   = 0.0f;
    float       min_token_p    = 0.0f;
    float       confidence     = 0.0f;   // mean_token_p * (1 - no_speech_prob)
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
};

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
