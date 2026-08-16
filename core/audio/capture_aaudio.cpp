// AAudio capture backend (Android, API 26+): wraps the NDK AAudio input path
// behind the CaptureBackend seam. Default input device only — whisper's
// selector shows a single "Micrófono" entry, mirroring how desktop JACK works
// (one entry for the whole server).
#ifdef __ANDROID__

#include "core/audio/capture_backends.h"
#include "core/audio/capture_convert.h"

#include <aaudio/AAudio.h>
#include <android/log.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "whisper_aaudio", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "whisper_aaudio", __VA_ARGS__)

namespace audio {
namespace detail {

namespace {

class AaudioBackend final : public CaptureBackend {
public:
    ~AaudioBackend() override { stop(); }

    std::string start() override
    {
        if (running_.load(std::memory_order_acquire)) return "Ya hay una captura en curso.";

        // Ask for whisper's native rate; the device negotiates the nearest it
        // supports and AAudioStream_getSampleRate reports the effective value.
        AAudioStreamBuilder * builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
            LOGE("createStreamBuilder failed");
            return "No se pudo crear el stream de audio.";
        }
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setSampleRate(builder, kTargetSampleRate);
        AAudioStreamBuilder_setChannelCount(builder, 1);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);

        aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);
        if (r != AAUDIO_OK || !stream_) {
            LOGW("openStream failed: %s", AAudio_convertResultToText(r));
            stream_ = nullptr;
            return "No se pudo abrir el micrófono (AAudio).";
        }

        rate_      = AAudioStream_getSampleRate(stream_);
        channels_  = AAudioStream_getChannelCount(stream_);
        // AAudio offers no getBytesPerSample; derive it from the negotiated
        // format. We ask for PCM_I16, but honor whatever came back.
        subslot_   = AAudioStream_getFormat(stream_) == AAUDIO_FORMAT_PCM_FLOAT ? 4 : 2;

        if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
            LOGE("requestStart failed");
            AAudioStream_close(stream_);
            stream_ = nullptr;
            return "No se pudo iniciar la captura del micrófono.";
        }

        buffer_ = std::make_shared<std::vector<float>>();
        abort_reason_.clear();
        stop_flag_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&AaudioBackend::run, this);
        return "";
    }

    void stop() override
    {
        stop_flag_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        peak_.store(0.0f, std::memory_order_release);
        running_.store(false, std::memory_order_release);
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
        Resampler16k resampler(rate_);
        if (!resampler.ok()) {
            abort_reason_ = "Error al inicializar el resampler (soxr).";
            finish();
            return;
        }

        // 50 ms of frames at a time; blocking read caps at ~100 ms.
        const int frame_bytes = subslot_ * channels_;
        if (frame_bytes <= 0) {
            abort_reason_ = "Formato de captura inválido.";
            finish();
            return;
        }
        const int num_frames = kTargetSampleRate / 20;
        std::vector<uint8_t> read_buf((size_t) num_frames * (size_t) frame_bytes);
        std::vector<float>   float_buf;

        while (!stop_flag_.load(std::memory_order_acquire)) {
            aaudio_result_t r = AAudioStream_read(
                stream_, read_buf.data(), num_frames, 100LL * 1000 * 1000);
            if (r < 0) {
                abort_reason_ = "Error al leer del micrófono.";
                break;
            }
            if (r == 0) continue;

            float_buf.clear();
            float chunk_peak = pcm_to_float_mono(read_buf.data(),
                                                 (size_t) r * (size_t) frame_bytes,
                                                 subslot_, channels_, float_buf);
            peak_.store(chunk_peak, std::memory_order_release);
            resampler.process(float_buf.data(), float_buf.size(), *buffer_);
        }

        resampler.finish(*buffer_);
        finish();
    }

    void finish()
    {
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        peak_.store(0.0f, std::memory_order_release);
        running_.store(false, std::memory_order_release);
    }

    AAudioStream *                      stream_ = nullptr;
    int                                 rate_ = kTargetSampleRate;
    int                                 channels_ = 1;
    int                                 subslot_ = 2;
    std::shared_ptr<std::vector<float>> buffer_;
    std::thread                         thread_;
    std::atomic<bool>                   running_{false};
    std::atomic<bool>                   stop_flag_{false};
    std::atomic<float>                  peak_{0.0f};
    std::string                         abort_reason_;
};

} // namespace

std::vector<CaptureDeviceInfo> enumerate_aaudio()
{
    // Single entry for the default input — matches the JACK backend's shape.
    return {{BackendKind::Aaudio, "", "Micrófono (AAudio)"}};
}

std::unique_ptr<CaptureBackend> make_aaudio(const std::string & /*device_id*/)
{
    return std::make_unique<AaudioBackend>();
}

} // namespace detail
} // namespace audio

#endif // __ANDROID__
