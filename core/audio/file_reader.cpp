#include "core/audio/file_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "core/audio/capture_convert.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_libs/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_libs/dr_flac.h"

namespace audio {

namespace {

std::string lower_ext(const std::string & path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    return ext;
}

// Downmixes interleaved float frames to mono by averaging, in place-ish.
std::vector<float> downmix(const float * frames, uint64_t frame_count, unsigned channels)
{
    std::vector<float> mono(frame_count);
    if (channels <= 1) {
        std::copy(frames, frames + frame_count, mono.begin());
        return mono;
    }
    for (uint64_t i = 0; i < frame_count; ++i) {
        float sum = 0.0f;
        for (unsigned c = 0; c < channels; ++c) sum += frames[i * channels + c];
        mono[i] = sum / channels;
    }
    return mono;
}

} // namespace

bool is_supported_audio_extension(const std::string & path)
{
    std::string ext = lower_ext(path);
    return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

std::shared_ptr<std::vector<float>> load_audio_file(const std::string & path,
                                                    std::string * err)
{
    std::string ext = lower_ext(path);

    unsigned channels = 0;
    unsigned rate     = 0;
    uint64_t frames   = 0;
    // Interleaved float, dr_libs-allocated. With null allocation callbacks
    // every dr_* decoder allocates with malloc, so std::free releases it.
    float *  pcm      = nullptr;

    if (ext == ".wav") {
        drwav_uint64 n = 0;
        pcm = drwav_open_file_and_read_pcm_frames_f32(
                  path.c_str(), &channels, &rate, &n, nullptr);
        frames = n;
        if (!pcm) {
            if (err) *err = "No se pudo leer el archivo WAV:\n" + path;
            return nullptr;
        }
    } else if (ext == ".mp3") {
        drmp3_config cfg{};
        drmp3_uint64 n = 0;
        pcm = drmp3_open_file_and_read_pcm_frames_f32(
                  path.c_str(), &cfg, &n, nullptr);
        if (!pcm) {
            if (err) *err = "No se pudo leer el archivo MP3:\n" + path;
            return nullptr;
        }
        channels = cfg.channels;
        rate     = cfg.sampleRate;
        frames   = n;
    } else if (ext == ".flac") {
        drflac_uint64 n = 0;
        pcm = drflac_open_file_and_read_pcm_frames_f32(
                  path.c_str(), &channels, &rate, &n, nullptr);
        frames = n;
        if (!pcm) {
            if (err) *err = "No se pudo leer el archivo FLAC:\n" + path;
            return nullptr;
        }
    } else {
        if (err) *err = "Formato no soportado (" + (ext.empty() ? std::string("sin extensión") : ext)
                      + "). Formatos válidos: .wav, .mp3, .flac.";
        return nullptr;
    }

    if (frames == 0 || channels == 0 || rate == 0) {
        std::free(pcm);
        if (err) *err = "El archivo de audio está vacío o corrupto:\n" + path;
        return nullptr;
    }

    std::vector<float> mono = downmix(pcm, frames, channels);
    std::free(pcm);

    auto out = std::make_shared<std::vector<float>>(
                   resample_to_16k(mono, (int) rate));
    if (out->empty()) {
        if (err) *err = "Fallo al remuestrear el audio a 16 kHz.";
        return nullptr;
    }
    return out;
}

} // namespace audio
