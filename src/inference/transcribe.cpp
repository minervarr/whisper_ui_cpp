#include "transcribe.h"
#include "confidence.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

#include "whisper.h"

namespace inference {

namespace {

std::atomic<int> g_active_workers{0};

std::wstring utf8_to_wide(const char * s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

int auto_thread_count() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    return (int) std::min(n, 8u);
}

// Construye whisper_full_params desde un Settings.
// IMPORTANTE: los strings (language, initial_prompt) viven en el Settings que pasamos,
// por eso lo capturamos por valor en el hilo y no movemos los buffers.
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
    p.token_timestamps = true;   // requerido para la métrica de confianza

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
                      HWND hwnd_notify) {
    if (!ctx || !samples || !hwnd_notify) return;

    cfg::Settings owned_settings = settings;  // copia: strings sobreviven en el hilo

    g_active_workers.fetch_add(1, std::memory_order_acq_rel);
    std::thread([ctx, samples = std::move(samples),
                 settings_copy = std::move(owned_settings),
                 hwnd_notify]() mutable {
        struct Decrement {
            ~Decrement() { g_active_workers.fetch_sub(1, std::memory_order_acq_rel); }
        } dec;

        auto * result = new Result();

        if (samples->empty()) {
            result->error = L"El buffer de audio está vacío. Pulsa Grabar y habla antes de detener.";
            PostMessageW(hwnd_notify, WM_TRANSCRIBE_DONE, 1, (LPARAM) result);
            return;
        }

        whisper_full_params p = build_params_from(settings_copy);

        int rc = whisper_full(ctx, p, samples->data(), (int) samples->size());
        if (rc != 0) {
            result->error = L"whisper_full devolvió un código de error (" + std::to_wstring(rc) + L").";
            PostMessageW(hwnd_notify, WM_TRANSCRIBE_DONE, 1, (LPARAM) result);
            return;
        }

        const int n_seg = whisper_full_n_segments(ctx);
        result->segments.reserve((size_t) n_seg);
        for (int i = 0; i < n_seg; ++i) {
            Segment seg;
            seg.t0_ms = whisper_full_get_segment_t0(ctx, i) * 10;  // centisegundos → ms
            seg.t1_ms = whisper_full_get_segment_t1(ctx, i) * 10;
            const char * txt = whisper_full_get_segment_text(ctx, i);
            seg.text = utf8_to_wide(txt);
            if (!seg.text.empty() && seg.text.front() == L' ') seg.text.erase(0, 1);
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
            if (lang_str) result->detected_language = utf8_to_wide(lang_str);
        }

        compute_confidence(ctx, *result);

        PostMessageW(hwnd_notify, WM_TRANSCRIBE_DONE, 0, (LPARAM) result);
    }).detach();
}

void join_pending_workers() {
    // Espera activa con pequeño sleep: el shutdown es raro y la lógica simple
    // evita orquestar joins de hilos detached por nombre/handle.
    while (g_active_workers.load(std::memory_order_acquire) > 0) {
        Sleep(50);
    }
}

} // namespace inference
