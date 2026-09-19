#include "core/transcribe.h"
#include "core/confidence.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

// ── Anti-hallucination: silence trimming (Layer 1) ──────────────────────────

std::vector<SpeechSpan> detect_speech_spans(const std::vector<float> & samples,
                                            float threshold, int min_silence_ms,
                                            int min_speech_ms)
{
    constexpr int64_t kSampleRate = 16000;
    constexpr int64_t kWindow     = 512;   // 32 ms @ 16 kHz
    const int64_t total = (int64_t) samples.size();
    if (total < kWindow) return {};

    const int64_t pad     = 300 * kSampleRate / 1000;                  // edge keep
    const int64_t min_gap = (int64_t) min_silence_ms * kSampleRate / 1000;
    const int64_t min_len = (int64_t) min_speech_ms  * kSampleRate / 1000;

    // Per-window RMS -> speech flag (measured reference: room tone ~0.0005,
    // speech ~0.02+; 0.004 sits safely between them).
    const size_t n_win = ((size_t) total + (size_t) kWindow - 1) / (size_t) kWindow;
    std::vector<bool> is_speech(n_win, false);
    for (size_t w = 0; w < n_win; ++w) {
        const size_t a = w * (size_t) kWindow;
        const size_t b = std::min(a + (size_t) kWindow, samples.size());
        double sum = 0.0;
        for (size_t k = a; k < b; ++k) sum += (double) samples[k] * (double) samples[k];
        const size_t cnt = b - a;
        if (cnt > 0 && std::sqrt(sum / (double) cnt) > (double) threshold)
            is_speech[w] = true;
    }

    // Raw runs of consecutive speech windows.
    std::vector<SpeechSpan> raw;
    size_t w = 0;
    while (w < n_win) {
        if (!is_speech[w]) { ++w; continue; }
        const int64_t a = (int64_t) w * kWindow;
        size_t e = w;
        while (e < n_win && is_speech[e]) ++e;
        const int64_t b = std::min((int64_t) e * kWindow, total);
        raw.push_back({a, b});
        w = e;
    }
    if (raw.empty()) return {};

    // Pad both edges, merge gaps under min_silence_ms, drop too-short spans.
    std::vector<SpeechSpan> out;
    int64_t cur_a = std::max<int64_t>(0, raw[0].begin - pad);
    int64_t cur_b = std::min(total, raw[0].end + pad);
    for (size_t i = 1; i < raw.size(); ++i) {
        const int64_t a = std::max<int64_t>(0, raw[i].begin - pad);
        const int64_t b = std::min(total, raw[i].end + pad);
        if (a <= cur_b + min_gap) {
            cur_b = std::max(cur_b, b);
        } else {
            if (cur_b - cur_a >= min_len) out.push_back({cur_a, cur_b});
            cur_a = a;
            cur_b = b;
        }
    }
    if (cur_b - cur_a >= min_len) out.push_back({cur_a, cur_b});
    return out;
}

ClipMap build_clip_map(const std::vector<float> & samples,
                       const std::vector<SpeechSpan> & spans,
                       std::vector<float> * out_clipped)
{
    ClipMap m;
    out_clipped->clear();
    out_clipped->reserve((size_t)
        (spans.empty() ? 0 : (spans.back().end - spans.front().begin)));
    int64_t clipped = 0;
    m.kept.reserve(spans.size());
    m.clipped_end.reserve(spans.size());
    for (const auto & sp : spans) {
        if (sp.end <= sp.begin) continue;
        const int64_t len = sp.end - sp.begin;
        out_clipped->insert(out_clipped->end(),
                            samples.begin() + sp.begin, samples.begin() + sp.end);
        m.kept.push_back({sp.begin, sp.end});
        clipped += len;
        m.clipped_end.push_back(clipped);
    }
    return m;
}

int64_t ClipMap::map_ms(int64_t clipped_ms) const
{
    if (kept.empty()) return 0;
    if (clipped_ms < 0) return 0;
    constexpr int64_t kSampleRate = 16000;
    const int64_t cs = clipped_ms * kSampleRate / 1000;  // clipped sample index
    // First kept span whose cumulative end > cs (binary search).
    size_t lo = 0, hi = clipped_end.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (clipped_end[mid] <= cs) lo = mid + 1; else hi = mid;
    }
    const size_t idx = std::min(lo, kept.size() - 1);
    const int64_t span_start = idx == 0 ? 0 : clipped_end[idx - 1];
    int64_t orig = kept[idx].begin + (cs - span_start);
    if (orig > kept[idx].end) orig = kept[idx].end;
    return orig * 1000 / kSampleRate;
}

std::vector<CallUnit> build_call_units(const std::vector<SpeechSpan> & spans,
                                       int64_t max_group_samples)
{
    std::vector<CallUnit> units;
    if (spans.empty() || max_group_samples <= 0) return units;

    int64_t off = 0;  // cumulative kept-audio offset (sample index)
    int64_t cur = 0;  // samples accumulated in the unit under construction
    for (const auto & sp : spans) {
        const int64_t len = sp.end - sp.begin;
        if (len <= 0) continue;

        // A single span longer than the budget: flush whatever is pending and
        // slice the span into budget-sized pieces.
        if (len > max_group_samples) {
            if (cur > 0) {
                units.push_back({off - cur, cur});
                cur = 0;
            }
            int64_t rem = len;
            while (rem > 0) {
                const int64_t take = std::min(rem, max_group_samples);
                units.push_back({off, take});
                off    += take;
                rem    -= take;
            }
            continue;
        }

        if (cur > 0 && cur + len > max_group_samples) {
            units.push_back({off - cur, cur});
            cur = 0;
        }
        cur += len;
        off += len;
    }
    if (cur > 0) units.push_back({off - cur, cur});
    return units;
}

// ── Anti-hallucination: repetition-run guard (Layer 2) ──────────────────────

void mark_repetition_runs(std::vector<Segment> & segments)
{
    const size_t n = segments.size();
    size_t i = 0;
    while (i < n) {
        // Period-1: >= 3 consecutive identical texts.
        size_t j = i + 1;
        while (j < n && segments[j].text == segments[i].text) ++j;
        if (j - i >= 3) {
            for (size_t k = i + 1; k < j; ++k) segments[k].rep_run = true;
            i = j;
            continue;
        }
        // Period-2: >= 6 segments alternating the same two texts (ABABAB —
        // the "Y… / si es que…" loop shape seen in the wild).
        if (i + 5 < n && segments[i].text != segments[i + 1].text) {
            size_t L = 2;
            while (i + L < n &&
                   segments[i + L].text == (L % 2 == 0 ? segments[i].text
                                                        : segments[i + 1].text))
                ++L;
            if (L >= 6) {
                for (size_t k = i + 2; k < i + L; ++k) segments[k].rep_run = true;
                i += L;
                continue;
            }
        }
        ++i;
    }
}

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

        // Layer 1: silence trimming. Energy VAD removes long silent gaps before
        // whisper ever sees them (its #1 hallucination trigger on long audio).
        // The kept audio is transcribed with a series of whisper_full calls,
        // one per bounded group of speech spans (build_call_units), so no single
        // decode has to survive minutes of continuous speech — on long
        // recordings whisper degenerates into a repetition loop mid-file (in
        // the wild: "Y… / si es que…", or latching onto the last phrase heard),
        // and a fresh call per region recovers the real speech. whisper keeps
        // only the last call's segments in context and resets its prompt per
        // call, so both confidence indexing and the anti-loop isolation come
        // free. Every timestamp is relative to each call's own buffer and gets
        // mapped back to the absolute file timeline.
        std::vector<float> clipped;
        ClipMap clip_map;
        const std::vector<float> * pcm = samples.get();
        bool trimmed = false;
        std::vector<SpeechSpan> spans;
        if (settings_copy.silence_trim) {
            spans = detect_speech_spans(*samples);
            int64_t kept_samples = 0;
            for (const auto & s : spans) kept_samples += s.end - s.begin;
            if (!spans.empty() && kept_samples < (int64_t) samples->size()) {
                clip_map = build_clip_map(*samples, spans, &clipped);
                pcm = &clipped;
                trimmed = true;
            }
        }

        const int64_t kGroupBudgetSamples = 90 * 16000;  // 90 s of audio per call
        const std::vector<CallUnit> units = trimmed
            ? build_call_units(spans, kGroupBudgetSamples)
            : std::vector<CallUnit>{{0, (int64_t) samples->size()}};

        const auto map_time = [&](int64_t clipped_ms) -> int64_t {
            return trimmed ? clip_map.map_ms(clipped_ms) : clipped_ms;
        };

        // The default language is "auto": whisper re-runs the expensive
        // language detection on every call. Detect once on the first call, then
        // pin the detected code for the remaining groups.
        std::string locked_lang;
        const bool auto_lang = settings_copy.language.empty() ||
                               settings_copy.language == "auto";

        const auto t0 = std::chrono::steady_clock::now();
        result->segments.reserve((size_t) (samples->size() / 16000));
        for (size_t gu = 0; gu < units.size(); ++gu) {
            whisper_full_params pc = p;  // per-call copy: language may be pinned
            if (gu > 0 && auto_lang && !locked_lang.empty()) {
                pc.language = locked_lang.c_str();
            }

            const int rc = whisper_full(ctx, pc,
                                        pcm->data() + units[gu].clip_offset,
                                        (int) units[gu].clip_len);
            if (rc != 0) {
                result->error = "whisper_full devolvió un código de error (" + std::to_string(rc) + ").";
                q->push({core::AppEvent::TranscribeDone, result});
                return;
            }

            if (auto_lang && locked_lang.empty()) {
                const int lid = whisper_full_lang_id(ctx);
                if (lid >= 0) locked_lang = whisper_lang_str(lid);
            }

            const int n_seg = whisper_full_n_segments(ctx);
            const int64_t base_ms = units[gu].clip_offset * 1000 / 16000;
            const size_t first_seg = result->segments.size();
            for (int i = 0; i < n_seg; ++i) {
                Segment seg;
                seg.t0_ms = map_time(base_ms + whisper_full_get_segment_t0(ctx, i) * 10);  // centiseconds -> ms
                seg.t1_ms = map_time(base_ms + whisper_full_get_segment_t1(ctx, i) * 10);
                const char * txt = whisper_full_get_segment_text(ctx, i);
                if (txt) seg.text = txt;
                if (!seg.text.empty() && seg.text.front() == ' ') seg.text.erase(0, 1);
                result->segments.push_back(std::move(seg));
            }

            // Token-level detail for the max-info mode. Same special-token rule
            // as confidence.cpp (id >= tok_eot); times are centiseconds * 10
            // like the segments above.
            if (settings_copy.keep_tokens) {
                const whisper_token tok_eot = whisper_token_eot(ctx);
                for (int i = 0; i < n_seg; ++i) {
                    const int n_tokens = whisper_full_n_tokens(ctx, i);
                    auto & seg = result->segments[first_seg + (size_t) i];
                    seg.tokens.reserve((size_t) n_tokens);
                    for (int t = 0; t < n_tokens; ++t) {
                        whisper_token_data td = whisper_full_get_token_data(ctx, i, t);
                        TokenInfo tok;
                        const char * tok_txt = whisper_full_get_token_text(ctx, i, t);
                        tok.text      = tok_txt ? tok_txt : "";
                        tok.id        = (int) td.id;
                        tok.p         = td.p;
                        tok.plog      = td.plog;
                        tok.t0_ms     = map_time(base_ms + td.t0 * 10);
                        tok.t1_ms     = map_time(base_ms + td.t1 * 10);
                        tok.vlen      = td.vlen;
                        tok.is_special = td.id >= tok_eot;
                        seg.tokens.push_back(std::move(tok));
                    }
                }
            }

            // Per-segment confidence: whisper keeps only THIS call's segments
            // in its context (result_all is cleared at every call), so indexes
            // line up exactly with the group we just pulled.
            for (int i = 0; i < n_seg; ++i) {
                compute_segment_confidence(ctx, i,
                                           result->segments[first_seg + (size_t) i]);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        result->transcribe_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        result->settings_used = settings_copy;

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

        // Layer 2: flag consecutive repetition runs so human-readable outputs
        // can collapse them (guard for when trimming is off or a loop survives).
        mark_repetition_runs(result->segments);

        aggregate_confidence(*result);

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
