#pragma once

#include <memory>
#include <string>
#include <vector>

namespace audio {

// True for the extensions the dr_libs decoders cover (.wav .mp3 .flac),
// case-insensitive.
bool is_supported_audio_extension(const std::string & path);

// Decodes an audio file to mono float 16 kHz (whisper's input format).
// On failure returns nullptr and fills *err with a Spanish message.
std::shared_ptr<std::vector<float>> load_audio_file(const std::string & path,
                                                    std::string * err);

} // namespace audio
