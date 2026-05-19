#pragma once

#include <atomic>
#include <string>
#include <thread>

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

    // Arranca la carga asíncrona del modelo.
    // El path se busca en <dir-del-exe>/models/ aceptando cualquier .bin o .gguf
    // (orden alfabético si hay varios).
    void start();

    // Nombre de fichero (sin ruta) del modelo elegido, para mostrar en la UI.
    std::wstring model_filename() const { return model_filename_; }

    LoadState state() const { return state_.load(std::memory_order_acquire); }

    // Mensaje de error en español si state() == Failed. Vacío en otros casos.
    std::wstring error_message() const { return error_msg_; }

    // Path completo que se intentó cargar (para mensajes al usuario).
    std::wstring model_path() const { return model_path_; }

    // Cierto si el contexto cargado usa GPU (Vulkan). Cierto solo cuando state() == Loaded.
    bool gpu_used() const { return gpu_used_.load(std::memory_order_acquire); }

    // Contexto whisper listo para inferencia. Válido solo cuando state() == Loaded.
    whisper_context * context() const { return ctx_.load(std::memory_order_acquire); }

private:
    void worker();

    std::atomic<LoadState>     state_{LoadState::NotStarted};
    std::atomic<whisper_context *> ctx_{nullptr};
    std::atomic<bool>          gpu_used_{false};
    std::wstring               error_msg_;
    std::wstring               model_path_;
    std::wstring               model_filename_;
    std::thread                thread_;
};

} // namespace inference
