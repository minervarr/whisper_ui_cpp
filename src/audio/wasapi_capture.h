#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace audio {

// Pico de muestra absoluto del último frame procesado (0.0 a ~1.0+).
// Escrito por el hilo de captura, leído por el hilo de UI para alimentar el LED.
extern std::atomic<float> g_peak;

// Tasa de muestreo objetivo entregada al consumidor (whisper.cpp espera 16 kHz).
constexpr int kTargetSampleRate = 16000;

class Capture {
public:
    Capture();
    ~Capture();

    Capture(const Capture &) = delete;
    Capture & operator=(const Capture &) = delete;

    // Arranca la captura desde el dispositivo configurado en device_id_
    // (vacío = default del sistema). Devuelve string vacío en éxito, o
    // mensaje de error en español en fallo.
    std::wstring start();

    // Reanuda la captura tras un aborto (mic desconectado) usando otro
    // dispositivo, MANTENIENDO el buffer ya grabado. Los nuevos samples se
    // anexan al final. Pre-condición: is_running() == false (el hilo previo
    // ya salió por su cuenta; no llamar take_buffer() entre el aborto y
    // este método si se quiere reanudar).
    std::wstring resume_with_device(const std::wstring & new_id);

    // Detiene la captura. Bloquea hasta que el hilo de captura termina.
    // Tras esto, take_buffer() entrega el audio grabado.
    void stop();

    // Selecciona el endpoint a usar en la próxima llamada a start().
    // "" = usar el default del sistema. No tiene efecto sobre una captura
    // ya en curso (usar resume_with_device para cambiar en caliente).
    void set_device_id(std::wstring id) { device_id_ = std::move(id); }

    // Endpoint id actualmente configurado ("" = default del sistema).
    std::wstring device_id() const { return device_id_; }

    // Transfiere la propiedad del buffer acumulado al llamante.
    // Solo llamar después de stop(). Resetea el estado interno.
    std::shared_ptr<std::vector<float>> take_buffer();

    // Duración del audio en el buffer actual (segundos).
    double duration_seconds() const;

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // Si el hilo de captura abortó por su cuenta (p. ej. mic desconectado),
    // este string contiene el motivo en español. Vacío si no hubo aborto.
    std::wstring abort_reason() const { return abort_reason_; }

private:
    std::wstring start_impl(bool keep_existing_buffer);
    void run_capture();

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    void *            event_handle_ = nullptr;  // HANDLE creado por el hilo de captura
    std::thread       thread_;
    std::shared_ptr<std::vector<float>> buffer_;
    std::wstring      thread_error_;   // error de inicio
    std::wstring      abort_reason_;   // aborto durante la captura (mic invalidated, etc.)
    std::wstring      device_id_;      // "" = default del sistema
};

} // namespace audio
