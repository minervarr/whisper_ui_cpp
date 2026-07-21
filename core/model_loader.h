#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "core/event_queue.h"

struct whisper_context;

namespace inference {

enum class LoadState {
    NotStarted,
    Loading,
    Loaded,
    Failed,
};

class ModelLoader {
public:
    ModelLoader();
    ~ModelLoader();

    ModelLoader(const ModelLoader &) = delete;
    ModelLoader & operator=(const ModelLoader &) = delete;

    // Starts the async model load. On completion pushes a ModelLoaded or
    // ModelFailed AppEvent into `queue`; the atomic state() getter stays
    // valid for status-line polling.
    // The model path is <exe_dir>/models/, first *.bin|*.gguf alphabetically.
    void start(core::EventQueue & queue);

    // Filename (no path) of the chosen model, for the UI.
    std::string model_filename() const { return model_filename_; }

    LoadState state() const { return state_.load(std::memory_order_acquire); }

    // Spanish error message when state() == Failed. Empty otherwise.
    std::string error_message() const { return error_msg_; }

    // Full path that was attempted (for user-facing messages).
    std::string model_path() const { return model_path_; }

    // True when the loaded context uses GPU (Vulkan). Only when Loaded.
    bool gpu_used() const { return gpu_used_.load(std::memory_order_acquire); }

    // whisper context ready for inference. Valid only when state() == Loaded.
    whisper_context * context() const { return ctx_.load(std::memory_order_acquire); }

private:
    void worker(core::EventQueue * queue);

    std::atomic<LoadState>         state_{LoadState::NotStarted};
    std::atomic<whisper_context *> ctx_{nullptr};
    std::atomic<bool>              gpu_used_{false};
    std::string                    error_msg_;
    std::string                    model_path_;
    std::string                    model_filename_;
    std::thread                    thread_;
};

} // namespace inference
