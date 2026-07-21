#include "core/transcribe.h"
#include "core/confidence.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "whisper.h"

namespace inference {

namespace {

std::atomic<int> g_active_workers{0};

int auto_thread_count() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    return (int) std::min(n, 8u);
}

// Builds whisper_full_params from a Settings.
// The strings (language, initial_prompt) live in the Settings copy captured
// by value in the worker thread, so the char* pointers stay valid.
whisper_full_params build_params_from(const cfg::Settings & s) {
    const auto strategy = s.use_beam_search ? WHISPER_SAMPLING_BEAM_SEARCH
                                            : WHISPER_SAMPLING_GREEDY;
    whisper_full_params p = whisper_full_default_params(strategy);

    p.language        = s.language.empty() ? "auto" : s.language.c_str();
    p.translate       = s.translate;
    p.detect_language = s.detect_language;
    p.n_threads       = s.n_threads > 0 ? s.n_threads : auto_thread_count();

    p.print_realtime  = s.print_realtime;
    p.print_progress  = s.print_progress;
    p.print_timestamps = false;
    p.token_timestamps = true;   // required by the confidence metric

    p.suppress_blank  = s.suppress_blank;
    p.suppress_nst    = s.suppress_nst;
    p.single_segment  = s.single_segment;
    p.split_on_word   = s.split_on_word;

    p.temperature      = s.temperature;
    p.temperature_inc  = s.temperature_inc;
    p.length_penalty   = s.length_penalty;
    p.entropy_thold    = s.entropy_thold;
    p.logprob_thold    = s.logprob_thold;
    p.no_speech_thold  = s.no_speech_thold;

    if (s.n_max_text_ctx > 0) p.n_max_text_ctx = s.n_max_text_ctx;
    if (s.max_len        > 0) p.max_len        = s.max_len;
    if (s.max_tokens     > 0) p.max_tokens     = s.max_tokens;
    if (s.audio_ctx      > 0) p.audio_ctx      = s.audio_ctx;

    p.initial_prompt        = s.initial_prompt.empty() ? nullptr : s.initial_prompt.c_str();
    p.carry_initial_prompt  = s.carry_initial_prompt;

    p.tdrz_enable = s.tdrz_enable;

    if (s.use_beam_search) p.beam_search.beam_size = s.beam_size;
    p.greedy.best_of = s.best_of;

    return p;
}

} // namespace

void transcribe_async(whisper_context * ctx,
                      std::shared_ptr<std::vector<float>> samples,
                      const cfg::Settings & settings,
                      core::EventQueue & queue) {
    if (!ctx || !samples) return;

    cfg::Settings owned_settings = settings;  // copy: strings outlive in the thread

    g_active_workers.fetch_add(1, std::memory_order_acq_rel);
    std::thread([ctx, samples = std::move(samples),
                 settings_copy = std::move(owned_settings),
                 q = &queue]() mutable {
        struct Decrement {
            ~Decrement() { g_active_workers.fetch_sub(1, std::memory_order_acq_rel); }
        } dec;

        auto * result = new Result();

        if (samples->empty()) {
            result->error = "El buffer de audio está vacío. Pulsa Grabar y habla antes de detener.";
            q->push({core::AppEvent::TranscribeDone, result});
            return;
        }

        whisper_full_params p = build_params_from(settings_copy);

        int rc = whisper_full(ctx, p, samples->data(), (int) samples->size());
        if (rc != 0) {
            result->error = "whisper_full devolvió un código de error (" + std::to_string(rc) + ").";
            q->push({core::AppEvent::TranscribeDone, result});
            return;
        }

        const int n_seg = whisper_full_n_segments(ctx);
        result->segments.reserve((size_t) n_seg);
        for (int i = 0; i < n_seg; ++i) {
            Segment seg;
            seg.t0_ms = whisper_full_get_segment_t0(ctx, i) * 10;  // centiseconds -> ms
            seg.t1_ms = whisper_full_get_segment_t1(ctx, i) * 10;
            const char * txt = whisper_full_get_segment_text(ctx, i);
            if (txt) seg.text = txt;
            if (!seg.text.empty() && seg.text.front() == ' ') seg.text.erase(0, 1);
            result->segments.push_back(std::move(seg));
        }

        if (!result->segments.empty()) {
            result->total_duration_ms = result->segments.back().t1_ms;
        } else {
            result->total_duration_ms = int64_t(samples->size()) * 1000 / 16000;
        }

        int lang_id = whisper_full_lang_id(ctx);
        if (lang_id >= 0) {
            const char * lang_str = whisper_lang_str(lang_id);
            if (lang_str) result->detected_language = lang_str;
        }

        compute_confidence(ctx, *result);

        q->push({core::AppEvent::TranscribeDone, result});
    }).detach();
}

void join_pending_workers() {
    // Busy-wait with a small sleep: shutdown is rare and this avoids
    // orchestrating joins of detached threads by name/handle.
    while (g_active_workers.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace inference
