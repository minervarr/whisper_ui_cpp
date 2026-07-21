// JACK2 capture backend: client of the user's already-running jackd (real
// libjack only — never pipewire-jack). Wraps audio_engine's JackCaptureDriver.
#ifndef _WIN32

#include "core/audio/capture_backends.h"
#include "core/audio/capture_convert.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "jack_capture.h"

namespace audio {
namespace detail {

namespace {

class JackBackend final : public CaptureBackend {
public:
    ~JackBackend() override { stop(); }

    std::string start() override
    {
        if (running_.load(std::memory_order_acquire)) return "Ya hay una captura en curso.";

        if (!driver_.open("whisper_destilado")) {
            return "El servidor JACK no está corriendo. Arráncalo (p. ej. con "
                   "qjackctl) y vuelve a intentarlo.";
        }
        // Mono capture; rate/bits are hints — the server dictates float32
        // at its own rate.
        if (!driver_.configureCapture(kTargetSampleRate, 1, 32)) {
            driver_.close();
            return "No se pudieron registrar los puertos de entrada JACK.";
        }
        if (!driver_.startCapture()) {
            driver_.close();
            return "No se pudo conectar a los puertos de captura del servidor JACK.";
        }

        buffer_ = std::make_shared<std::vector<float>>();
        abort_reason_.clear();
        stop_flag_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&JackBackend::run, this);
        return "";
    }

    void stop() override
    {
        stop_flag_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    std::shared_ptr<std::vector<float>> take_buffer() override
    {
        auto b = buffer_;
        buffer_.reset();
        return b;
    }

    float peak() const override { return peak_.load(std::memory_order_acquire); }
    bool  running() const override { return running_.load(std::memory_order_acquire); }
    std::string abort_reason() const override { return abort_reason_; }

private:
    void run()
    {
        const int rate = driver_.getConfiguredCaptureRate();

        Resampler16k resampler(rate);
        if (!resampler.ok()) {
            abort_reason_ = "Error al inicializar el resampler (soxr).";
            finish();
            return;
        }

        // JACK delivers interleaved float32 (mono here) — no int conversion.
        std::vector<uint8_t> read_buf(8192);

        while (!stop_flag_.load(std::memory_order_acquire)) {
            int n = driver_.readCapture(read_buf.data(), (int) read_buf.size());
            if (n < 0) {
                abort_reason_ = "Se perdió la conexión con el servidor JACK.";
                break;
            }
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const size_t samples = (size_t) n / sizeof(float);
            const float * f = reinterpret_cast<const float *>(read_buf.data());

            float chunk_peak = 0.0f;
            for (size_t i = 0; i < samples; ++i) {
                float a = std::fabs(f[i]);
                if (a > chunk_peak) chunk_peak = a;
            }
            peak_.store(chunk_peak, std::memory_order_release);

            resampler.process(f, samples, *buffer_);
        }

        resampler.finish(*buffer_);
        finish();
    }

    void finish()
    {
        driver_.stopCapture();
        driver_.close();
        peak_.store(0.0f, std::memory_order_release);
        running_.store(false, std::memory_order_release);
    }

    JackCaptureDriver                   driver_;
    std::shared_ptr<std::vector<float>> buffer_;
    std::thread                         thread_;
    std::atomic<bool>                   running_{false};
    std::atomic<bool>                   stop_flag_{false};
    std::atomic<float>                  peak_{0.0f};
    std::string                         abort_reason_;
};

} // namespace

bool jack_server_reachable()
{
    JackCaptureDriver probe;
    if (!probe.open("whisper_destilado_probe")) return false;
    probe.close();
    return true;
}

std::unique_ptr<CaptureBackend> make_jack()
{
    return std::make_unique<JackBackend>();
}

} // namespace detail
} // namespace audio

#endif // !_WIN32
