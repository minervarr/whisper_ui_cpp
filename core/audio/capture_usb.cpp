// Direct USB-DAC capture backend (UAC1/UAC2 over libusb) via audio_engine's
// UsbAudioDriver. Works on Linux and Windows alike.

#include "core/audio/capture_backends.h"
#include "core/audio/capture_convert.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "usb_audio.h"

namespace audio {
namespace detail {

namespace {

bool parse_usb_id(const std::string & id, uint16_t * vid, uint16_t * pid)
{
    unsigned v = 0, p = 0;
    if (std::sscanf(id.c_str(), "LIBUSB:%x:%x", &v, &p) != 2) return false;
    *vid = (uint16_t) v;
    *pid = (uint16_t) p;
    return true;
}

class UsbBackend final : public CaptureBackend {
public:
    explicit UsbBackend(std::string device_id) : device_id_(std::move(device_id)) {}

    ~UsbBackend() override { stop(); }

    std::string start() override
    {
        if (running_.load(std::memory_order_acquire)) return "Ya hay una captura en curso.";

        uint16_t vid = 0, pid = 0;
        if (!parse_usb_id(device_id_, &vid, &pid)) {
            return "Identificador USB inválido: " + device_id_;
        }

        if (!driver_.open(vid, pid)) {
            return "No se pudo abrir el dispositivo USB DAC. "
                   "¿Permisos udev correctos?";
        }
        if (!driver_.parseDescriptors()) {
            driver_.close();
            return "Fallo al leer los descriptores del USB DAC.";
        }

        auto tuples = driver_.getCaptureFormatTuples();
        if (tuples.empty()) {
            driver_.close();
            return "El dispositivo USB DAC no reporta formatos de captura.";
        }
        // Prefer whisper's native 16 kHz mono int16; else first advertised.
        int rate = tuples[0], ch = tuples[1], bits = tuples[2];
        for (size_t i = 0; i + 2 < tuples.size(); i += 3) {
            if (tuples[i] == kTargetSampleRate && tuples[i + 1] == 1 && tuples[i + 2] == 16) {
                rate = kTargetSampleRate; ch = 1; bits = 16;
                break;
            }
        }

        if (!driver_.configureCapture(rate, ch, bits)) {
            driver_.close();
            return "El USB DAC no aceptó la configuración de captura.";
        }
        if (!driver_.startCapture()) {
            driver_.close();
            return "No se pudo iniciar la captura en el USB DAC.";
        }

        buffer_ = std::make_shared<std::vector<float>>();
        abort_reason_.clear();
        stop_flag_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&UsbBackend::run, this);
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
                abort_reason_ = "Error al leer del USB DAC.";
                break;
            }
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

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
    UsbAudioDriver                      driver_;
    std::shared_ptr<std::vector<float>> buffer_;
    std::thread                         thread_;
    std::atomic<bool>                   running_{false};
    std::atomic<bool>                   stop_flag_{false};
    std::atomic<float>                  peak_{0.0f};
    std::string                         abort_reason_;
};

} // namespace

std::vector<CaptureDeviceInfo> enumerate_usb()
{
    std::vector<CaptureDeviceInfo> out;
    for (const auto & d : UsbAudioDriver::enumerateUsbAudioDevices()) {
        char id[32];
        std::snprintf(id, sizeof id, "LIBUSB:%04x:%04x", d.vid, d.pid);
        out.push_back({BackendKind::Usb, id, d.name + " (USB directo)"});
    }
    return out;
}

std::unique_ptr<CaptureBackend> make_usb(const std::string & device_id)
{
    return std::make_unique<UsbBackend>(device_id);
}

} // namespace detail
} // namespace audio
