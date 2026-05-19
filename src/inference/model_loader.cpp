#include "model_loader.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "whisper.h"

namespace inference {

namespace {

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
}

std::string wide_to_utf8(const std::wstring & w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool is_model_file(const std::filesystem::path & p) {
    if (!std::filesystem::is_regular_file(p)) return false;
    auto ext = p.extension().wstring();
    for (auto & c : ext) c = (wchar_t) towlower(c);
    return ext == L".bin" || ext == L".gguf";
}

// Escanea la carpeta `models` adyacente al .exe y devuelve la primera
// ruta de modelo (.bin o .gguf) en orden alfabético, o vacío si no hay ninguno.
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
    auto dir = exe_dir() / L"models";
    auto picked = pick_model(dir);
    if (!picked.empty()) {
        model_path_     = picked.wstring();
        model_filename_ = picked.filename().wstring();
    } else {
        // Aún sin modelo; guardamos el path del directorio para el mensaje de error.
        model_path_     = dir.wstring();
        model_filename_.clear();
    }
}

ModelLoader::~ModelLoader() {
    if (thread_.joinable()) thread_.join();
    whisper_context * c = ctx_.exchange(nullptr);
    if (c) whisper_free(c);
}

void ModelLoader::start() {
    LoadState expected = LoadState::NotStarted;
    if (!state_.compare_exchange_strong(expected, LoadState::Loading)) return;
    thread_ = std::thread(&ModelLoader::worker, this);
}

void ModelLoader::worker() {
    // Si no se encontró ningún modelo al construir, reescaneamos por si el
    // usuario lo colocó después (raro pero barato).
    if (model_filename_.empty()) {
        auto dir = exe_dir() / L"models";
        auto picked = pick_model(dir);
        if (!picked.empty()) {
            model_path_     = picked.wstring();
            model_filename_ = picked.filename().wstring();
        }
    }

    if (model_filename_.empty()) {
        error_msg_ = L"No se encontró ningún modelo en:\n\n"
                   + model_path_
                   + L"\n\nColoca un archivo .bin o .gguf de whisper "
                     L"(por ejemplo ggml-large-v3.bin, ggml-medium.bin, "
                     L"ggml-base.en.bin, etc.) dentro de la carpeta 'models' "
                     L"junto al ejecutable.";
        state_.store(LoadState::Failed, std::memory_order_release);
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(model_path_, ec)) {
        error_msg_ = L"El modelo dejó de existir entre el arranque y la carga:\n\n"
                   + model_path_;
        state_.store(LoadState::Failed, std::memory_order_release);
        return;
    }

    const std::string utf8_path = wide_to_utf8(model_path_);

    // Intento 1: GPU (Vulkan).
    {
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        whisper_context * c = whisper_init_from_file_with_params(utf8_path.c_str(), cparams);
        if (c) {
            ctx_.store(c, std::memory_order_release);
            gpu_used_.store(true, std::memory_order_release);
            state_.store(LoadState::Loaded, std::memory_order_release);
            return;
        }
    }

    // Intento 2: CPU.
    {
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = false;
        whisper_context * c = whisper_init_from_file_with_params(utf8_path.c_str(), cparams);
        if (c) {
            ctx_.store(c, std::memory_order_release);
            gpu_used_.store(false, std::memory_order_release);
            state_.store(LoadState::Loaded, std::memory_order_release);
            return;
        }
    }

    error_msg_ = L"whisper.cpp no pudo cargar el modelo:\n\n"
               + model_path_
               + L"\n\nVerifica que el archivo no esté corrupto y que sea un "
                 L".bin o .gguf válido para whisper.cpp.";
    state_.store(LoadState::Failed, std::memory_order_release);
}

} // namespace inference
