#include "core/model_loader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "whisper.h"

namespace inference {

namespace {

std::filesystem::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
#else
    char buf[4096];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    return std::filesystem::path(buf).parent_path();
#endif
}

bool is_model_file(const std::filesystem::path & p) {
    if (!std::filesystem::is_regular_file(p)) return false;
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    return ext == ".bin" || ext == ".gguf";
}

// Scans the `models` folder next to the executable and returns the first
// model path (.bin or .gguf) alphabetically, or empty if there is none.
std::filesystem::path pick_model(const std::filesystem::path & models_dir) {
    std::vector<std::filesystem::path> candidates;
    std::error_code ec;
    if (!std::filesystem::exists(models_dir, ec) || !std::filesystem::is_directory(models_dir, ec)) {
        return {};
    }
    for (const auto & entry : std::filesystem::directory_iterator(models_dir, ec)) {
        if (is_model_file(entry.path())) candidates.push_back(entry.path());
    }
    if (candidates.empty()) return {};
    std::sort(candidates.begin(), candidates.end());
    return candidates.front();
}

} // namespace

ModelLoader::ModelLoader() {
    auto dir = exe_dir() / "models";
    auto picked = pick_model(dir);
    if (!picked.empty()) {
        model_path_     = picked.string();
        model_filename_ = picked.filename().string();
    } else {
        // No model yet; keep the directory path for the error message.
        model_path_     = dir.string();
        model_filename_.clear();
    }
}

ModelLoader::~ModelLoader() {
    if (thread_.joinable()) thread_.join();
    whisper_context * c = ctx_.exchange(nullptr);
    if (c) whisper_free(c);
}

void ModelLoader::start(core::EventQueue & queue) {
    LoadState expected = LoadState::NotStarted;
    if (!state_.compare_exchange_strong(expected, LoadState::Loading)) return;
    thread_ = std::thread(&ModelLoader::worker, this, &queue);
}

void ModelLoader::worker(core::EventQueue * queue) {
    // If no model was found at construction, rescan in case the user dropped
    // one in afterwards (rare but cheap).
    if (model_filename_.empty()) {
        auto dir = exe_dir() / "models";
        auto picked = pick_model(dir);
        if (!picked.empty()) {
            model_path_     = picked.string();
            model_filename_ = picked.filename().string();
        }
    }

    if (model_filename_.empty()) {
        error_msg_ = "No se encontró ningún modelo en:\n\n"
                   + model_path_
                   + "\n\nColoca un archivo .bin o .gguf de whisper "
                     "(por ejemplo ggml-large-v3.bin, ggml-medium.bin, "
                     "ggml-base.en.bin, etc.) dentro de la carpeta 'models' "
                     "junto al ejecutable.";
        state_.store(LoadState::Failed, std::memory_order_release);
        queue->push({core::AppEvent::ModelFailed, nullptr});
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(model_path_, ec)) {
        error_msg_ = "El modelo dejó de existir entre el arranque y la carga:\n\n"
                   + model_path_;
        state_.store(LoadState::Failed, std::memory_order_release);
        queue->push({core::AppEvent::ModelFailed, nullptr});
        return;
    }

    // Attempt 1: GPU (Vulkan).
    {
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        whisper_context * c = whisper_init_from_file_with_params(model_path_.c_str(), cparams);
        if (c) {
            ctx_.store(c, std::memory_order_release);
            gpu_used_.store(true, std::memory_order_release);
            state_.store(LoadState::Loaded, std::memory_order_release);
            queue->push({core::AppEvent::ModelLoaded, nullptr});
            return;
        }
    }

    // Attempt 2: CPU.
    {
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = false;
        whisper_context * c = whisper_init_from_file_with_params(model_path_.c_str(), cparams);
        if (c) {
            ctx_.store(c, std::memory_order_release);
            gpu_used_.store(false, std::memory_order_release);
            state_.store(LoadState::Loaded, std::memory_order_release);
            queue->push({core::AppEvent::ModelLoaded, nullptr});
            return;
        }
    }

    error_msg_ = "whisper.cpp no pudo cargar el modelo:\n\n"
               + model_path_
               + "\n\nVerifica que el archivo no esté corrupto y que sea un "
                 ".bin o .gguf válido para whisper.cpp.";
    state_.store(LoadState::Failed, std::memory_order_release);
    queue->push({core::AppEvent::ModelFailed, nullptr});
}

} // namespace inference
