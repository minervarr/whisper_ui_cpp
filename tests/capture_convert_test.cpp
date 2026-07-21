#include "harness.hpp"

#include "core/audio/capture_convert.h"

#include <cmath>
#include <cstring>
#include <vector>

TEST(int16_scaling_exact)
{
    int16_t samples[3] = {0, 32767, -32768};
    std::vector<float> out;
    float peak = audio::pcm_to_float_mono(
        reinterpret_cast<const uint8_t*>(samples), sizeof samples, 2, 1, out);
    CHECK_EQ(out.size(), (size_t) 3);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == 32767.0f / 32768.0f);
    CHECK(out[2] == -1.0f);
    CHECK(peak == 1.0f);
}

TEST(stereo_downmix_average)
{
    // L=0.5FS, R=-0.25FS -> mono 0.125FS
    int16_t frames[4] = {16384, -8192, 16384, -8192};
    std::vector<float> out;
    audio::pcm_to_float_mono(
        reinterpret_cast<const uint8_t*>(frames), sizeof frames, 2, 2, out);
    CHECK_EQ(out.size(), (size_t) 2);
    CHECK(std::fabs(out[0] - 0.125f) < 1e-6f);
    CHECK(std::fabs(out[1] - 0.125f) < 1e-6f);
}

TEST(int24_scaling)
{
    // +0x400000 (0.5FS) and -0x400000, packed little-endian.
    uint8_t bytes[6] = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0};
    std::vector<float> out;
    audio::pcm_to_float_mono(bytes, sizeof bytes, 3, 1, out);
    CHECK_EQ(out.size(), (size_t) 2);
    CHECK(std::fabs(out[0] - 0.5f) < 1e-6f);
    CHECK(std::fabs(out[1] + 0.5f) < 1e-6f);
}

TEST(resample_48k_to_16k_keeps_frequency)
{
    // 1 second of a 440 Hz sine at 48 kHz.
    const int in_rate = 48000;
    const double freq = 440.0;
    std::vector<float> in(in_rate);
    for (int i = 0; i < in_rate; ++i)
        in[(size_t) i] = (float) std::sin(2.0 * M_PI * freq * i / in_rate);

    std::vector<float> out = audio::resample_to_16k(in, in_rate);

    // Length ~16000 (soxr may trim a few samples at the edges).
    CHECK(out.size() > 15800 && out.size() <= 16100);

    // Frequency preserved: count zero crossings — a 440 Hz sine over ~1 s
    // has ~880 crossings; allow ±2%.
    int crossings = 0;
    for (size_t i = 1; i < out.size(); ++i)
        if ((out[i - 1] < 0.0f) != (out[i] < 0.0f)) ++crossings;
    double expected = 2.0 * freq * out.size() / audio::kTargetSampleRate;
    CHECK(std::fabs(crossings - expected) < expected * 0.02);

    // Amplitude survives.
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.9f && peak < 1.1f);
}

TEST(resample_16k_passthrough)
{
    std::vector<float> in = {0.1f, -0.2f, 0.3f};
    std::vector<float> out = audio::resample_to_16k(in, 16000);
    CHECK_EQ(out.size(), in.size());
    CHECK(out == in);
}
