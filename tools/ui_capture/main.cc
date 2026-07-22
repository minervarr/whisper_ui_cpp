// whisper_ui_capture — headless PNG snapshots of every UI state (DEBUG ONLY).
//
// Builds each scenario as a synthetic DrawState + RecorderController and draws
// it through the real gui::draw_main, then vkc::capture_main renders it
// off-screen and writes a lossless PNG tree. Nomenclature: NN-label segments,
// '/' for parent/child. Deterministic — no live devices, fixed inputs.
//
//   whisper_ui_capture --out ui-flow [--frame WxH] [--scale N] [--only S] [--list]

#include "canvas.hh"
#include "renderer.hh"
#include "wayland_platform.hh"   // FileAssetReader

#include "capture.hh"   // vk_canvas_capture (framework/vk_canvas/core/capture)

#include "core/event_queue.h"
#include "core/languages.h"
#include "core/transcribe.h"

#include "gui/fonts.h"
#include "gui/recorder_controller.h"
#include "gui/views.h"

#include <string>
#include <vector>

namespace {

using gui::DrawState;
using gui::RecorderController;

// ── small builders for synthetic states ─────────────────────────────────────

void to_ready(RecorderController& c)
{
    c.on_model_event({core::AppEvent::ModelLoaded, nullptr}, "");
}

DrawState base_state(RecorderController& ctl)
{
    DrawState st;
    st.ctl        = &ctl;
    st.lang_label = "Auto-detectar idioma";
    st.mic_label  = "HD-Audio Generic — ALC257 Analog";
    st.format_sel = 1;   // srt
    st.save_path  = "/home/nava/transcripcion.txt";
    return st;
}

// A finished two-segment transcription (Good tier), owned by the controller.
inference::Result* make_result()
{
    auto* r = new inference::Result();
    r->detected_language  = "es";
    r->confidence_overall = 0.82f;
    r->tier               = inference::ConfidenceTier::Good;

    inference::Segment s1;
    s1.t0_ms = 0; s1.t1_ms = 2500;
    s1.text  = "Hola, esto es una prueba de transcripción.";
    inference::Segment s2;
    s2.t0_ms = 2500; s2.t1_ms = 5200;
    s2.text  = "El sistema de dictado funciona correctamente.";
    r->segments = { s1, s2 };
    return r;
}

// Draw one state through the real view code and submit the frame.
void present(Renderer& r, DrawState& st)
{
    std::vector<float> curves, quads;
    Canvas c = gui::make_canvas(curves, quads, r.width(), r.height());
    std::vector<gui::Hit> hits;
    gui::draw_main(c, st, hits);
    r.draw(curves, 0, {}, {}, quads);
}

// A recording state at the given input peak (drives the LED color).
void present_recording(Renderer& r, float peak)
{
    RecorderController ctl;
    to_ready(ctl);
    ctl.tick(0, 0.0f);           // seed now_ms_ = 0
    ctl.on_record_pressed();     // Ready -> Recording (record_start_ms_ = 0)
    ctl.tick(7000, peak);        // 00:07 elapsed, peak sets the level dot
    DrawState st = base_state(ctl);
    present(r, st);
}

// A finished-result state, optionally with a toast / focused path field.
void present_result(Renderer& r, const std::string& toast, bool path_focused)
{
    RecorderController ctl;
    to_ready(ctl);
    ctl.on_record_pressed();     // -> Recording
    ctl.on_record_pressed();     // -> Transcribing
    ctl.on_transcribe_done(make_result());   // -> Ready, result present
    DrawState st = base_state(ctl);
    st.toast        = toast;
    st.path_focused = path_focused;
    present(r, st);
}

// A device/language picker popup, hovering item `hover` (-1 = none).
void present_popup(Renderer& r, DrawState::Popup kind,
                   const std::vector<std::string>& items, int hover)
{
    RecorderController ctl;
    to_ready(ctl);
    DrawState st = base_state(ctl);
    st.popup          = kind;
    st.popup_items    = &items;
    st.popup_selected = 0;
    st.popup_scroll   = 0.0f;
    if (hover >= 0) {
        // Center of item `hover`: popup_area = {W*.15, H*.12, W*.70, H*.72},
        // row height = H*.055 (gui::popup_row_height).
        float H = (float)r.height(), W = (float)r.width();
        st.ptr = { W * 0.5f, H * 0.12f + (hover + 0.5f) * gui::popup_row_height(H), false };
    }
    present(r, st);
}

std::vector<std::string> language_items()
{
    std::vector<std::string> v;
    for (const auto& l : languages::all()) v.push_back(l.label);
    return v;
}

std::vector<std::string> mic_items()
{
    return {
        "HD-Audio Generic — ALC257 Analog",
        "Servidor JACK",
        "USB Audio DAC (USB directo)",
    };
}

std::vector<vkc::Scenario> scenarios()
{
    return {
        { "00-loading", [](Renderer& r) {
            RecorderController ctl;                 // Loading (fresh)
            DrawState st = base_state(ctl);
            present(r, st);
        }},
        { "10-ready", [](Renderer& r) {
            RecorderController ctl; to_ready(ctl);
            DrawState st = base_state(ctl);
            present(r, st);
        }},
        { "20-recording/00-ok",   [](Renderer& r) { present_recording(r, 0.10f); }},
        { "20-recording/10-warn", [](Renderer& r) { present_recording(r, 0.85f); }},
        { "20-recording/20-clip", [](Renderer& r) { present_recording(r, 0.97f); }},
        { "30-transcribing", [](Renderer& r) {
            RecorderController ctl; to_ready(ctl);
            ctl.on_record_pressed();                // Recording
            ctl.on_record_pressed();                // Transcribing
            DrawState st = base_state(ctl);
            present(r, st);
        }},
        { "40-result/00-transcript",     [](Renderer& r) { present_result(r, "", false); }},
        { "40-result/10-toast-guardado", [](Renderer& r) {
            present_result(r, "Guardado: /home/nava/transcripcion.srt", false); }},
        { "40-result/20-path-focused",   [](Renderer& r) { present_result(r, "", true); }},
        { "50-popup/00-idioma", [](Renderer& r) {
            auto items = language_items();
            present_popup(r, DrawState::Popup::Lang, items, -1); }},
        { "50-popup/10-idioma-hover", [](Renderer& r) {
            auto items = language_items();
            present_popup(r, DrawState::Popup::Lang, items, 3); }},
        { "50-popup/20-microfono", [](Renderer& r) {
            auto items = mic_items();
            present_popup(r, DrawState::Popup::Mic, items, -1); }},
        { "90-error", [](Renderer& r) {
            RecorderController ctl;
            ctl.on_model_event({core::AppEvent::ModelFailed, nullptr},
                "No se encontró ningún modelo en la carpeta 'models'. Coloca un "
                "archivo .bin o .gguf de whisper junto al ejecutable.");
            DrawState st = base_state(ctl);
            present(r, st);
        }},
    };
}

} // namespace

int main(int argc, char** argv)
{
    vkc::CaptureConfig cfg;
    cfg.default_out = "ui-flow";
    // Runs once, only when a Renderer is actually created (so --list stays
    // instant): load the curve font + MSDF atlas, then upload it.
    cfg.init = [](Renderer& r) {
        FileAssetReader assets;
        std::string cache = gui::msdf_cache_path();
        gui::init_fonts(assets, cache);
        gui::upload_msdf(r, cache);
    };

    return vkc::capture_main(argc, argv, scenarios(), cfg);
}
