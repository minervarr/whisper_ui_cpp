#include "harness.hpp"

#include "core/audio/file_reader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void put_u32(std::ofstream & f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
void put_u16(std::ofstream & f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }

// Writes a minimal RIFF/WAVE file: 16-bit PCM, interleaved.
void write_wav(const std::string & path, const std::vector<int16_t> & samples,
               int rate, int channels)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    uint32_t data_bytes = (uint32_t)(samples.size() * 2);
    f.write("RIFF", 4);
    put_u32(f, 36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put_u32(f, 16);
    put_u16(f, 1);                                    // PCM
    put_u16(f, (uint16_t) channels);
    put_u32(f, (uint32_t) rate);
    put_u32(f, (uint32_t)(rate * channels * 2));      // byte rate
    put_u16(f, (uint16_t)(channels * 2));             // block align
    put_u16(f, 16);                                   // bits per sample
    f.write("data", 4);
    put_u32(f, data_bytes);
    f.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
}

} // namespace

TEST(supported_extensions)
{
    CHECK(audio::is_supported_audio_extension("a.wav"));
    CHECK(audio::is_supported_audio_extension("A.MP3"));
    CHECK(audio::is_supported_audio_extension("/x/y.FlAc"));
    CHECK(!audio::is_supported_audio_extension("a.m4a"));
    CHECK(!audio::is_supported_audio_extension("noext"));
}

TEST(load_stereo_wav_to_mono_16k)
{
    // 1 s of a 440 Hz sine, 44.1 kHz stereo (same signal both channels).
    const int rate = 44100;
    const double freq = 440.0;
    std::vector<int16_t> samples((size_t) rate * 2);
    for (int i = 0; i < rate; ++i) {
        int16_t v = (int16_t)(0.5 * 32767.0 * std::sin(2.0 * M_PI * freq * i / rate));
        samples[(size_t) i * 2]     = v;
        samples[(size_t) i * 2 + 1] = v;
    }

    fs::path wav = fs::temp_directory_path() / "wd_file_reader_test.wav";
    write_wav(wav.string(), samples, rate, 2);

    std::string err;
    auto buf = audio::load_audio_file(wav.string(), &err);
    fs::remove(wav);

    CHECK(buf != nullptr);
    CHECK_EQ(err, std::string(""));
    if (!buf) return;

    // ~16000 mono samples.
    CHECK(buf->size() > 15800 && buf->size() <= 16100);

    float peak = 0.0f;
    for (float v : *buf) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.4f && peak < 0.6f);

    // Frequency check via zero crossings (~880 for 1 s of 440 Hz).
    int crossings = 0;
    for (size_t i = 1; i < buf->size(); ++i)
        if (((*buf)[i - 1] < 0.0f) != ((*buf)[i] < 0.0f)) ++crossings;
    double expected = 2.0 * freq * buf->size() / 16000.0;
    CHECK(std::fabs(crossings - expected) < expected * 0.02);
}

TEST(load_missing_file_fails_with_spanish_error)
{
    std::string err;
    auto buf = audio::load_audio_file("/nonexistent/nope.wav", &err);
    CHECK(buf == nullptr);
    CHECK(!err.empty());
    CHECK(err.find("WAV") != std::string::npos);
}

TEST(load_unsupported_extension_fails)
{
    std::string err;
    auto buf = audio::load_audio_file("whatever.m4a", &err);
    CHECK(buf == nullptr);
    CHECK(err.find(".m4a") != std::string::npos);
}
