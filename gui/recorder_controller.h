#pragma once

// Pure-logic recording state machine — no audio, no UI, no clock of its own.
// The App owns the side effects (starting/stopping capture, launching the
// transcription) and reports back through the on_* methods; views render
// exclusively from the getters. Fully unit-testable.

#include <cstdint>
#include <memory>
#include <string>

#include "core/confidence.h"
#include "core/event_queue.h"
#include "core/transcribe.h"

namespace gui {

enum class UiState {
    Loading,        // model still loading
    Ready,
    Recording,
    Transcribing,
    Error,          // model failed — terminal
};

class RecorderController {
public:
    // What the App must do after a record-button press.
    enum class Action {
        None,
        StartCapture,
        StopAndTranscribe,
    };

    // Record button pressed. Ready -> Recording (StartCapture),
    // Recording -> Transcribing (StopAndTranscribe); otherwise None.
    Action on_record_pressed();

    // TranscribeDone arrived; takes ownership of the result.
    void on_transcribe_done(inference::Result * result);

    // ModelLoaded / ModelFailed passed through from the event queue.
    // `error` is the loader's Spanish message (used on failure).
    void on_model_event(const core::AppEvent & ev, const std::string & error);

    // Capture died mid-take (device unplugged, server gone...).
    void on_capture_aborted(const std::string & reason);

    // Once per frame: `now_ms` monotonic milliseconds, `peak` current
    // capture peak 0..1 (ignored outside Recording).
    void tick(int64_t now_ms, float peak);

    // --- Getters for the views ---
    UiState     state() const           { return state_; }
    std::string status_line() const     { return status_; }
    std::string timer_text() const;                          // "MM:SS"
    uint32_t    led_color() const;                           // inference::rgb packing
    std::string transcript_preview() const;                  // joined segments or ""
    const inference::Result * last_result() const { return last_result_.get(); }

private:
    UiState  state_ = UiState::Loading;
    std::string status_ = "Cargando modelo…";
    int64_t  record_start_ms_ = 0;
    int64_t  now_ms_ = 0;
    float    peak_ = 0.0f;
    std::unique_ptr<inference::Result> last_result_;
};

} // namespace gui
