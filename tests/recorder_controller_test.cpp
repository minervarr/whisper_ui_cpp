#include "harness.hpp"

#include "gui/recorder_controller.h"

using gui::RecorderController;
using gui::UiState;

namespace {

RecorderController ready_controller()
{
    RecorderController c;
    c.on_model_event({core::AppEvent::ModelLoaded, nullptr}, "");
    return c;
}

} // namespace

TEST(full_state_walk)
{
    RecorderController c;
    CHECK(c.state() == UiState::Loading);
    CHECK(c.on_record_pressed() == RecorderController::Action::None);

    c.on_model_event({core::AppEvent::ModelLoaded, nullptr}, "");
    CHECK(c.state() == UiState::Ready);
    CHECK_EQ(c.status_line(), std::string("Listo para grabar."));

    CHECK(c.on_record_pressed() == RecorderController::Action::StartCapture);
    CHECK(c.state() == UiState::Recording);

    CHECK(c.on_record_pressed() == RecorderController::Action::StopAndTranscribe);
    CHECK(c.state() == UiState::Transcribing);
    CHECK(c.on_record_pressed() == RecorderController::Action::None);

    auto * r = new inference::Result();
    inference::Segment s;
    s.text = "hola";
    r->segments.push_back(s);
    r->tier = inference::ConfidenceTier::Excellent;
    c.on_transcribe_done(r);
    CHECK(c.state() == UiState::Ready);
    CHECK_EQ(c.transcript_preview(), std::string("hola"));
    CHECK(c.status_line().find("Excelente") != std::string::npos);
}

TEST(model_failure_is_terminal)
{
    RecorderController c;
    c.on_model_event({core::AppEvent::ModelFailed, nullptr}, "no hay modelo");
    CHECK(c.state() == UiState::Error);
    CHECK_EQ(c.status_line(), std::string("no hay modelo"));
    CHECK(c.on_record_pressed() == RecorderController::Action::None);
}

TEST(peak_to_led_mapping)
{
    RecorderController c = ready_controller();
    CHECK_EQ(c.led_color(), inference::rgb(60, 160, 60));   // Ready

    c.on_record_pressed();
    c.tick(1000, 0.10f);
    CHECK_EQ(c.led_color(), inference::rgb(40, 220, 40));   // RecOk
    c.tick(1100, 0.85f);
    CHECK_EQ(c.led_color(), inference::rgb(240, 200, 40));  // RecWarn (>=0.80)
    c.tick(1200, 0.97f);
    CHECK_EQ(c.led_color(), inference::rgb(230, 40, 40));   // RecClip (>=0.95)

    c.on_record_pressed();
    CHECK_EQ(c.led_color(), inference::rgb(40, 120, 230));  // Processing
}

TEST(timer_counts_only_while_recording)
{
    RecorderController c = ready_controller();
    CHECK_EQ(c.timer_text(), std::string("00:00"));

    c.tick(5000, 0.0f);
    c.on_record_pressed();      // start at now=5000
    c.tick(66000, 0.0f);        // 61 s later
    CHECK_EQ(c.timer_text(), std::string("01:01"));

    c.on_record_pressed();      // stop
    CHECK_EQ(c.timer_text(), std::string("00:00"));
}

TEST(abort_mid_record)
{
    RecorderController c = ready_controller();
    c.on_record_pressed();
    CHECK(c.state() == UiState::Recording);

    c.on_capture_aborted("Se desconectó el micrófono.");
    CHECK(c.state() == UiState::Ready);
    CHECK_EQ(c.status_line(), std::string("Se desconectó el micrófono."));
}

TEST(transcribe_error_returns_to_ready)
{
    RecorderController c = ready_controller();
    c.on_record_pressed();
    c.on_record_pressed();
    auto * r = new inference::Result();
    r->error = "whisper_full devolvió un código de error (1).";
    c.on_transcribe_done(r);
    CHECK(c.state() == UiState::Ready);
    CHECK_EQ(c.status_line(), r->error);
    CHECK_EQ(c.transcript_preview(), std::string(""));
}

TEST(empty_transcription_message)
{
    RecorderController c = ready_controller();
    c.on_record_pressed();
    c.on_record_pressed();
    c.on_transcribe_done(new inference::Result());
    CHECK(c.state() == UiState::Ready);
    CHECK_EQ(c.status_line(), std::string("Sin voz detectada. Listo para grabar."));
}
