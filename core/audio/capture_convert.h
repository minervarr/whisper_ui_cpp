#pragma once

#include <cstdint>
#include <vector>

namespace audio {

constexpr int kTargetSampleRate = 16000;

// Converts interleaved PCM (int16 / packed int24 / int32, little-endian) to
// mono float [-1,1] by channel averaging. Appends to `out`. Returns the peak
// absolute value of the appended samples.
float pcm_to_float_mono(const uint8_t * data, size_t bytes,
                        int bytes_per_sample, int channels,
                        std::vector<float> & out);

// Streaming soxr resampler: mono float at `in_rate` -> mono float 16 kHz.
// A pass-through when in_rate == 16000.
class Resampler16k {
public:
    explicit Resampler16k(int in_rate);
    ~Resampler16k();

    Resampler16k(const Resampler16k &) = delete;
    Resampler16k & operator=(const Resampler16k &) = delete;

    bool ok() const { return ok_; }

    // Resamples `in` and appends the produced samples to `out`.
    void process(const float * in, size_t n, std::vector<float> & out);

    // Flushes soxr's internal tail into `out`. Call once, at end of stream.
    void finish(std::vector<float> & out);

private:
    void * soxr_ = nullptr;   // soxr_t, kept opaque so the header stays clean
    int    in_rate_;
    bool   ok_ = false;
};

// One-shot convenience: mono float at `in_rate` -> mono float 16 kHz.
std::vector<float> resample_to_16k(const std::vector<float> & in, int in_rate);

} // namespace audio
