#include "gui/app.h"
#include "gui/host.hh"

#include "canvas.hh"
#include "font.hh"
#include "frame_input.hh"
#include "keys.hh"
#include "msdf.hh"
#include "renderer.hh"
#include "widgets.hh"

#include "core/audio/file_reader.h"
#include "core/confidence.h"
#include "core/event_queue.h"
#include "core/languages.h"
#include "core/output_format.h"
#include "core/transcribe.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace gui {

namespace {

// ── fonts (curve font + cached MSDF atlas, scanersito pipeline) ─────────────

Font     g_font;
bool     g_font_ok = false;
MsdfFont g_msdf;
bool     g_msdf_ok = false;

constexpr const char * kFontRegular = "fonts/NewCM10-Book.otf";

void init_fonts(AssetReader & assets, const std::string & cache)
{
    std::vector<uint8_t> bytes;
    if (assets.read(kFontRegular, bytes))
        g_font_ok = g_font.loadFromMemory(bytes.data(), bytes.size());

    if (g_msdf.generate(assets, kFontRegular, cache.c_str()))
        g_msdf_ok = g_msdf.valid();
    if (!g_msdf_ok)
        std::fprintf(stderr, "[!] MSDF unavailable — curve text only\n");
}

void upload_msdf(Renderer & r, const std::string & cache)
{
    if (!g_msdf_ok) return;
    g_msdf.ensureAtlasLoaded(cache.c_str());
    r.initMsdf(g_msdf);
    g_msdf.releaseAtlasPixels();
}

std::string msdf_cache_path()
{
    std::string cfg = cfg::config_path();          // ensures the dir exists
    if (cfg.empty()) return "msdf.cache";
    return std::filesystem::path(cfg).parent_path().string() + "/msdf.cache";
}

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

std::string default_save_path()
{
    const char * home = std::getenv("HOME");
#ifdef _WIN32
    home = std::getenv("USERPROFILE");
#endif
    return (home ? std::string(home) : std::string(".")) + "/transcripcion.txt";
}

// Replace the path's extension with the chosen format's.
std::string with_format_ext(const std::string & path, int format_sel)
{
    std::filesystem::path p(path);
    p.replace_extension(std::string(".") + kFormats[format_sel]);
    return p.string();
}

std::string format_result(const inference::Result & r, int format_sel)
{
    switch (format_sel) {
        case 1:  return inference::format_srt(r);
        case 2:  return inference::format_vtt(r);
        case 3:  return inference::format_json(r);
        default: return inference::format_txt(r);
    }
}

// 1 s 440 Hz sine at 16 kHz — the selftest's built-in "recording".
std::shared_ptr<std::vector<float>> selftest_sine()
{
    auto buf = std::make_shared<std::vector<float>>(16000);
    for (size_t i = 0; i < buf->size(); ++i)
        (*buf)[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / 16000.0f);
    return buf;
}

} // namespace

// ── side effects ────────────────────────────────────────────────────────────

void App::refresh_devices()
{
    devices_ = audio::enumerate_capture_devices();

    // Re-resolve the persisted choice against the current list.
    device_sel_ = -1;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if ((int) devices_[i].kind == settings_.capture_backend &&
            devices_[i].id == settings_.mic_device_id) {
            device_sel_ = (int) i;
            break;
        }
    }
    if (device_sel_ < 0 && !devices_.empty()) device_sel_ = 0;

    st_.mic_label = device_sel_ >= 0 ? devices_[(size_t) device_sel_].name
                                     : "(ninguno)";
}

std::string App::start_capture()
{
    if (device_sel_ < 0 || (size_t) device_sel_ >= devices_.size())
        return "No hay ningún dispositivo de captura disponible.";
    capture_ = audio::make_capture(devices_[(size_t) device_sel_]);
    if (!capture_) return "No se pudo crear el backend de captura.";
    return capture_->start();
}

void App::stop_and_transcribe()
{
    if (!capture_) return;
    capture_->stop();
    std::string abort_reason = capture_->abort_reason();
    last_take_ = capture_->take_buffer();
    capture_.reset();

    if (!abort_reason.empty() || !last_take_ || last_take_->empty()) {
        auto * r = new inference::Result();
        r->error = !abort_reason.empty()
                       ? abort_reason
                       : "El buffer de audio está vacío. Habla antes de detener.";
        ctl_.on_transcribe_done(r);
        return;
    }
    inference::transcribe_async(loader_.context(), last_take_, settings_,
                                core::events());
}

void App::retranscribe(bool quality)
{
    if (!last_take_ || last_take_->empty()) return;
    if (!ctl_.on_retry_started()) return;
    cfg::Settings s = quality ? settings_.with_quality_preset() : settings_;
    inference::transcribe_async(loader_.context(), last_take_, s, core::events());
}

void App::save_result()
{
    const inference::Result * r = ctl_.last_result();
    if (!r || !r->error.empty()) return;
    std::string path = with_format_ext(st_.save_path, st_.format_sel);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        st_.toast = "No se pudo escribir: " + path;
        return;
    }
    f << format_result(*r, st_.format_sel);
    st_.toast = f ? ("Guardado: " + path) : ("Error al escribir: " + path);
}

void App::select_language(int index)
{
    const auto & langs = languages::all();
    if (index < 0 || (size_t) index >= langs.size()) return;
    lang_sel_ = index;
    st_.lang_label = langs[(size_t) index].label;
    settings_.language = langs[(size_t) index].code;
    cfg::save_settings(settings_);
}

void App::select_device(int index)
{
    if (index < 0 || (size_t) index >= devices_.size()) return;
    device_sel_ = index;
    st_.mic_label = devices_[(size_t) index].name;
    settings_.capture_backend = (int) devices_[(size_t) index].kind;
    settings_.mic_device_id   = devices_[(size_t) index].id;
    cfg::save_settings(settings_);
}

void App::handle_hit(int action)
{
    if (action >= ActPopupBase && action < ActPopupClose) {
        int idx = action - ActPopupBase;
        if (st_.popup == DrawState::Popup::Lang) select_language(idx);
        else if (st_.popup == DrawState::Popup::Mic) select_device(idx);
        st_.popup = DrawState::Popup::None;
        return;
    }
    if (action == ActPopupClose) {
        st_.popup = DrawState::Popup::None;
        return;
    }
    if (action >= ActFormatBase && action < ActFormatBase + kFormatCount) {
        st_.format_sel = action - ActFormatBase;
        return;
    }

    st_.path_focused = false;
    switch (action) {
        case ActRecord: {
            auto a = ctl_.on_record_pressed();
            if (a == RecorderController::Action::StartCapture) {
                std::string err = start_capture();
                if (!err.empty()) {
                    capture_.reset();
                    ctl_.on_capture_aborted(err);
                }
            } else if (a == RecorderController::Action::StopAndTranscribe) {
                stop_and_transcribe();
            }
            break;
        }
        case ActLangField: {
            popup_items_.clear();
            for (const auto & l : languages::all()) popup_items_.push_back(l.label);
            st_.popup = DrawState::Popup::Lang;
            st_.popup_items = &popup_items_;
            st_.popup_selected = lang_sel_;
            st_.popup_scroll = 0.0f;
            break;
        }
        case ActMicField: {
            refresh_devices();
            popup_items_.clear();
            for (const auto & d : devices_) popup_items_.push_back(d.name);
            st_.popup = DrawState::Popup::Mic;
            st_.popup_items = &popup_items_;
            st_.popup_selected = device_sel_;
            st_.popup_scroll = 0.0f;
            break;
        }
        case ActPathField:      st_.path_focused = true; break;
        case ActSave:           save_result(); break;
        case ActRetryQuality:   retranscribe(true);  break;
        case ActRetryLang:      retranscribe(false); break;
        default: break;
    }
}

// ── selftest (headless, CI-able) ────────────────────────────────────────────

int App::run_selftest()
{
    loader_.start(core::events());
    while (loader_.state() == inference::LoadState::NotStarted ||
           loader_.state() == inference::LoadState::Loading)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    bool model_failed = false;
    for (auto & ev : core::events().drain()) {
        ctl_.on_model_event(ev, loader_.error_message());
        if (ev.kind == core::AppEvent::ModelFailed) model_failed = true;
    }
    if (model_failed || ctl_.state() != UiState::Ready) {
        std::printf("SELFTEST FAIL (modelo: %s)\n", loader_.error_message().c_str());
        return 1;
    }

    // The full controller path with the built-in sine as the "take".
    ctl_.on_record_pressed();                       // Ready -> Recording
    last_take_ = selftest_sine();
    ctl_.on_record_pressed();                       // Recording -> Transcribing
    inference::transcribe_async(loader_.context(), last_take_, settings_,
                                core::events());

    inference::Result * result = nullptr;
    while (!result) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (auto & ev : core::events().drain())
            if (ev.kind == core::AppEvent::TranscribeDone) result = ev.result;
    }
    ctl_.on_transcribe_done(result);
    inference::join_pending_workers();

    bool ok = ctl_.state() == UiState::Ready &&
              (!ctl_.last_result() || ctl_.last_result()->error.empty());
    std::printf("SELFTEST %s (estado: %s)\n", ok ? "OK" : "FAIL",
                ctl_.status_line().c_str());
    return ok ? 0 : 1;
}

// ── the frame loop ──────────────────────────────────────────────────────────

int App::run(bool selftest)
{
    settings_ = cfg::load_settings();

    // Restore language selection from settings.
    {
        const auto & langs = languages::all();
        for (size_t i = 0; i < langs.size(); ++i)
            if (settings_.language == langs[i].code) { lang_sel_ = (int) i; break; }
        st_.lang_label = langs[(size_t) lang_sel_].label;
    }
    st_.save_path = default_save_path();
    st_.ctl = &ctl_;

    if (selftest) return run_selftest();

    std::unique_ptr<AppHost> host = make_host();
    if (!host->init()) {
        std::fprintf(stderr, "[x] window creation failed\n");
        return 1;
    }

    const std::string cache = msdf_cache_path();
    init_fonts(host->assets(), cache);
    upload_msdf(*host->renderer(), cache);

    loader_.start(core::events());
    refresh_devices();

    FrameInput         input;
    std::vector<Hit>   hits;
    std::vector<float> curves;
    std::vector<float> msdf_quads;
    float              last_px = -1.0f, last_py = -1.0f;

    while (running_) {
        // 16 ms while the timer/level dot are live; lazy 50 ms otherwise.
        const bool live = ctl_.state() == UiState::Recording ||
                          ctl_.state() == UiState::Transcribing ||
                          ctl_.state() == UiState::Loading;
        input.beginFrame();
        host->pump(live ? 16 : 50, input);
        if (host->quit_requested()) break;
        if (host->take_dirty()) dirty_ = true;

        // Cross-thread events.
        for (auto & ev : core::events().drain()) {
            if (ev.kind == core::AppEvent::TranscribeDone) {
                ctl_.on_transcribe_done(ev.result);
            } else {
                ctl_.on_model_event(ev, loader_.error_message());
            }
            dirty_ = true;
        }

        // Capture death detection (device unplugged, jackd gone...).
        if (ctl_.state() == UiState::Recording && capture_ && !capture_->running()) {
            std::string reason = capture_->abort_reason();
            capture_.reset();
            ctl_.on_capture_aborted(reason);
            dirty_ = true;
        }

        ctl_.tick(now_ms(), capture_ ? capture_->peak() : 0.0f);
        if (live) dirty_ = true;

        // Keyboard.
        if (input.keyWentDown(key::Escape)) {
            if (st_.popup != DrawState::Popup::None) {
                st_.popup = DrawState::Popup::None;
                dirty_ = true;
            } else if (st_.path_focused) {
                st_.path_focused = false;
                dirty_ = true;
            } else {
                break;
            }
        }
        if (st_.path_focused) {
            if (!input.typedChars.empty()) {
                st_.save_path += input.typedChars;
                dirty_ = true;
            }
            if (input.keyWentDown(key::Backspace) && !st_.save_path.empty()) {
                st_.save_path.pop_back();
                dirty_ = true;
            }
            if (input.keyWentDown(key::Enter)) {
                st_.path_focused = false;
                dirty_ = true;
            }
        }

        // Wheel scrolls the open popup.
        if (st_.popup != DrawState::Popup::None && input.wheelDelta != 0.0f) {
            float row_h = popup_row_height((float) host->renderer()->height());
            st_.popup_scroll -= input.wheelDelta * row_h;
            float content = widgets::listContentHeight(
                (int) popup_items_.size(), row_h);
            float view_h = (float) host->renderer()->height() * 0.72f;
            float max_scroll = content > view_h ? content - view_h : 0.0f;
            if (st_.popup_scroll < 0) st_.popup_scroll = 0;
            if (st_.popup_scroll > max_scroll) st_.popup_scroll = max_scroll;
            dirty_ = true;
        }

        // Mouse: this frame's click against last frame's zones (1 frame of
        // lag is invisible at 60 fps).
        if (input.pointerWentDown) {
            st_.toast.clear();
            for (const Hit & hit : hits) {
                if (!hit.rect.contains(input.pointerX, input.pointerY)) continue;
                handle_hit(hit.action);
                break;
            }
            dirty_ = true;
        }
        if (input.pointerDown || input.pointerWentUp) dirty_ = true;

        // Pointer motion redraws so hover feedback tracks the cursor live.
        if (input.pointerX != last_px || input.pointerY != last_py) {
            last_px = input.pointerX;
            last_py = input.pointerY;
            dirty_ = true;
        }

        if (!dirty_) continue;
        dirty_ = false;

        st_.ptr = { input.pointerX, input.pointerY, input.pointerDown };

        curves.clear();
        msdf_quads.clear();
        Renderer * r = host->renderer();
        Canvas c(curves, r->width(), r->height(),
                 g_font_ok ? &g_font : nullptr, 0, 0, 0, 0);
        if (g_msdf_ok) c.useMsdf(&g_msdf, &msdf_quads);
        draw_main(c, st_, hits);
        r->draw(curves, 0, {}, {}, msdf_quads);
    }

    // Orderly shutdown: no capture thread or whisper worker may outlive us.
    if (capture_) { capture_->stop(); capture_.reset(); }
    inference::join_pending_workers();
    return 0;
}

} // namespace gui
