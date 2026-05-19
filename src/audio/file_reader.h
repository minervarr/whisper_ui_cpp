#pragma once

#include <memory>
#include <string>
#include <vector>

namespace audio {

// Decodes an audio file to 16 kHz mono IEEE float samples using Media Foundation.
// Supports WAV, MP3, AAC/M4A, WMA, FLAC (Win10+) and any format MF can decode.
// Returns nullptr on error; *out_error receives a Spanish description.
std::shared_ptr<std::vector<float>>
    load_audio_file(const std::wstring& path, std::wstring* out_error);

// Returns true when the file extension is one Media Foundation can decode.
bool is_supported_audio_extension(const std::wstring& path);

} // namespace audio
