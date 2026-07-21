#include "gui/recorder_controller.h"

#include <cstdio>

namespace gui {

namespace {

// LED palette carried over from the old led_control.cpp.
constexpr uint32_t kLedIdle       = inference::rgb(120, 120, 120);
constexpr uint32_t kLedReady      = inference::rgb( 60, 160,  60);
constexpr uint32_t kLedRecOk      = inference::rgb( 40, 220,  40);
constexpr uint32_t kLedRecWarn    = inference::rgb(240, 200,  40);
constexpr uint32_t kLedRecClip    = inference::rgb(230,  40,  40);
constexpr uint32_t kLedProcessing = inference::rgb( 40, 120, 230);
constexpr uint32_t kLedError      = inference::rgb(150,  40,  40);

// Peak thresholds from the old peak_to_led.
constexpr float kPeakWarn = 0.80f;
constexpr float kPeakClip = 0.95f;

} // namespace

RecorderController::Action RecorderController::on_record_pressed()
{
    switch (state_) {
        case UiState::Ready:
            state_ = UiState::Recording;
            record_start_ms_ = now_ms_;
            peak_ = 0.0f;
            status_ = "Grabando… pulsa de nuevo para detener.";
            return Action::StartCapture;
        case UiState::Recording:
            state_ = UiState::Transcribing;
            status_ = "Transcribiendo…";
            return Action::StopAndTranscribe;
        default:
            return Action::None;
    }
}

void RecorderController::on_transcribe_done(inference::Result * result)
{
    last_result_.reset(result);
    if (state_ == UiState::Error) return;   // model already dead — keep the error

    if (result && !result->error.empty()) {
        state_  = UiState::Ready;
        status_ = result->error;
        return;
    }
    state_ = UiState::Ready;
    if (result && result->segments.empty()) {
        status_ = "Sin voz detectada. Listo para grabar.";
    } else {
        char buf[128];
        std::snprintf(buf, sizeof buf, "Listo. Confianza: %s.",
                      result ? inference::tier_label(result->tier) : "-");
        status_ = buf;
    }
}

void RecorderController::on_model_event(const core::AppEvent & ev, const std::string & error)
{
    if (ev.kind == core::AppEvent::ModelLoaded) {
        if (state_ == UiState::Loading) {
            state_  = UiState::Ready;
            status_ = "Listo para grabar.";
        }
    } else if (ev.kind == core::AppEvent::ModelFailed) {
        state_  = UiState::Error;
        status_ = error.empty() ? "No se pudo cargar el modelo." : error;
    }
}

void RecorderController::on_capture_aborted(const std::string & reason)
{
    if (state_ != UiState::Recording) return;
    state_  = UiState::Ready;
    status_ = reason.empty() ? "La captura se interrumpió." : reason;
}

void RecorderController::tick(int64_t now_ms, float peak)
{
    now_ms_ = now_ms;
    peak_   = peak;
}

std::string RecorderController::timer_text() const
{
    int64_t elapsed = 0;
    if (state_ == UiState::Recording) elapsed = now_ms_ - record_start_ms_;
    if (elapsed < 0) elapsed = 0;
    int64_t total_s = elapsed / 1000;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld",
                  (long long)(total_s / 60), (long long)(total_s % 60));
    return buf;
}

uint32_t RecorderController::led_color() const
{
    switch (state_) {
        case UiState::Loading:      return kLedIdle;
        case UiState::Ready:        return kLedReady;
        case UiState::Recording:
            if (peak_ >= kPeakClip) return kLedRecClip;
            if (peak_ >= kPeakWarn) return kLedRecWarn;
            return kLedRecOk;
        case UiState::Transcribing: return kLedProcessing;
        case UiState::Error:        return kLedError;
    }
    return kLedIdle;
}

std::string RecorderController::transcript_preview() const
{
    if (!last_result_ || !last_result_->error.empty()) return "";
    std::string out;
    for (const auto & seg : last_result_->segments) {
        if (!out.empty()) out += "\n";
        out += seg.text;
    }
    return out;
}

} // namespace gui
