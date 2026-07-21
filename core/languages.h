#pragma once

#include <string>
#include <utility>
#include <vector>

namespace languages {

struct Language {
    const char * code;   // stable pointer (whisper-internal or the "auto" literal)
    std::string  label;  // "Spanish (es)" — UTF-8, lives in the static table
};

// Static table built once from whisper's language list, entry 0 = auto-detect.
// Never reallocated, so `code` pointers stay valid for the process lifetime.
const std::vector<Language> & all();

// Display label for a code ("es" -> "Spanish (es)"). Falls back to the code.
std::string name_for(const std::string & code);

} // namespace languages
