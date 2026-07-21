#include "core/audio/capture_convert.h"

#include <cmath>
#include <cstring>

#include "soxr.h"

namespace audio {

float pcm_to_float_mono(const uint8_t * data, size_t bytes,
                        int bytes_per_sample, int channels,
                        std::vector<float> & out)
{
    if (!data || bytes_per_sample < 2 || bytes_per_sample > 4 || channels < 1)
        return 0.0f;

    const size_t frame_bytes = (size_t) bytes_per_sample * channels;
    const size_t num_frames  = bytes / frame_bytes;

    out.reserve(out.size() + num_frames);

    float peak = 0.0f;
    for (size_t i = 0; i < num_frames; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) {
            const uint8_t * ptr = data + (i * channels + c) * bytes_per_sample;
            float val = 0.0f;
            if (bytes_per_sample == 2) {
                int16_t v;
                std::memcpy(&v, ptr, 2);
                val = v / 32768.0f;
            } else if (bytes_per_sample == 3) {
                int32_t v = (ptr[0]) | (ptr[1] << 8) | ((int8_t) ptr[2] << 16);
                val = v / 8388608.0f;
            } else {
                int32_t v;
                std::memcpy(&v, ptr, 4);
                val = v / 2147483648.0f;
            }
            sum += val;
        }
        float s = sum / channels;
        out.push_back(s);
        float a = std::fabs(s);
        if (a > peak) peak = a;
    }
    return peak;
}

Resampler16k::Resampler16k(int in_rate) : in_rate_(in_rate)
{
    if (in_rate == kTargetSampleRate) {
        ok_ = true;   // pass-through, no soxr instance
        return;
    }
    soxr_error_t err = nullptr;
    soxr_io_spec_t      io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t q  = soxr_quality_spec(SOXR_HQ, 0);
    soxr_ = soxr_create(in_rate, kTargetSampleRate, 1, &err, &io, &q, nullptr);
    ok_ = (soxr_ != nullptr && !err);
}

Resampler16k::~Resampler16k()
{
    if (soxr_) soxr_delete((soxr_t) soxr_);
}

void Resampler16k::process(const float * in, size_t n, std::vector<float> & out)
{
    if (!ok_ || n == 0) return;
    if (!soxr_) {   // pass-through
        out.insert(out.end(), in, in + n);
        return;
    }
    // Worst case output for this chunk plus slack for soxr's buffering.
    size_t max_out = n * (size_t) kTargetSampleRate / (size_t) in_rate_ + 128;
    size_t old = out.size();
    out.resize(old + max_out);
    size_t idone = 0, odone = 0;
    soxr_process((soxr_t) soxr_, in, n, &idone, out.data() + old, max_out, &odone);
    out.resize(old + odone);
}

void Resampler16k::finish(std::vector<float> & out)
{
    if (!ok_ || !soxr_) return;
    // Signal end-of-input; drain until soxr stops producing.
    for (;;) {
        size_t old = out.size();
        const size_t chunk = 4096;
        out.resize(old + chunk);
        size_t odone = 0;
        soxr_process((soxr_t) soxr_, nullptr, 0, nullptr, out.data() + old, chunk, &odone);
        out.resize(old + odone);
        if (odone < chunk) break;
    }
}

std::vector<float> resample_to_16k(const std::vector<float> & in, int in_rate)
{
    if (in_rate == kTargetSampleRate) return in;
    std::vector<float> out;
    Resampler16k rs(in_rate);
    if (!rs.ok()) return out;
    rs.process(in.data(), in.size(), out);
    rs.finish(out);
    return out;
}

} // namespace audio
