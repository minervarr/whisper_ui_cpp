// ALSA capture backend: wraps audio_engine's AlsaCaptureDriver (direct hw:
// device access, no sound server) behind the CaptureBackend seam.
#ifndef _WIN32

#include "core/audio/capture_backends.h"
#include "core/audio/capture_convert.h"

#include <atomic>
#include <thread>
#include <vector>

#include "alsa_capture.h"

namespace audio {
namespace detail {

namespace {

class AlsaBackend final : public CaptureBackend {
public:
    explicit AlsaBackend(std::string device_id) : device_id_(std::move(device_id)) {}

    ~AlsaBackend() override { stop(); }

    std::string start() override
    {
        if (running_.load(std::memory_order_acquire)) return "Ya hay una captura en curso.";

        if (!driver_.open(device_id_)) {
            return "No se pudo abrir el dispositivo ALSA (" + device_id_ +
                   "). ¿Está en uso por otro programa (p. ej. jackd)?";
        }
        // Ask for whisper's format; the driver negotiates the nearest the
        // hardware supports and the getters report the effective values.
        if (!driver_.configureCapture(kTargetSampleRate, 1, 16)) {
            driver_.close();
            return "El dispositivo ALSA no aceptó ninguna configuración de captura.";
        }
        if (!driver_.startCapture()) {
            driver_.close();
            return "No se pudo iniciar la captura ALSA.";
        }

        buffer_ = std::make_shared<std::vector<float>>();
        abort_reason_.clear();
        stop_flag_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&AlsaBackend::run, this);
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
        const int rate     = driver_.getConfiguredCaptureRate();
        const int channels = driver_.getConfiguredCaptureChannels();
        int subslot        = driver_.getConfiguredCaptureSubslotSize();
        if (subslot == 0) subslot = driver_.getConfiguredCaptureBitDepth() / 8;

        Resampler16k resampler(rate);
        if (!resampler.ok()) {
            abort_reason_ = "Error al inicializar el resampler (soxr).";
            finish();
            return;
        }

        std::vector<uint8_t> read_buf(4096);
        std::vector<float>   float_buf;

        while (!stop_flag_.load(std::memory_order_acquire)) {
            int n = driver_.readCapture(read_buf.data(), (int) read_buf.size());
            if (n < 0) {
                abort_reason_ = "Error al leer del dispositivo ALSA.";
                break;
            }
            if (n == 0) continue;   // readCapture already waited (<=100 ms)

            float_buf.clear();
            float chunk_peak = pcm_to_float_mono(read_buf.data(), (size_t) n,
                                                 subslot, channels, float_buf);
            peak_.store(chunk_peak, std::memory_order_release);
            resampler.process(float_buf.data(), float_buf.size(), *buffer_);
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

    std::string                         device_id_;
    AlsaCaptureDriver                   driver_;
    std::shared_ptr<std::vector<float>> buffer_;
    std::thread                         thread_;
    std::atomic<bool>                   running_{false};
    std::atomic<bool>                   stop_flag_{false};
    std::atomic<float>                  peak_{0.0f};
    std::string                         abort_reason_;
};

} // namespace

std::vector<CaptureDeviceInfo> enumerate_alsa()
{
    std::vector<CaptureDeviceInfo> out;
    for (const auto & d : AlsaCaptureDriver::enumerateCaptureDevices()) {
        out.push_back({BackendKind::Alsa, d.deviceId, d.name});
    }
    return out;
}

std::unique_ptr<CaptureBackend> make_alsa(const std::string & device_id)
{
    return std::make_unique<AlsaBackend>(device_id);
}

} // namespace detail
} // namespace audio

#endif // !_WIN32
