#include "core/languages.h"

#include <cctype>

#include "whisper.h"

namespace languages {

namespace {

std::string title_case(std::string s) {
    if (!s.empty()) s[0] = (char) std::toupper((unsigned char) s[0]);
    return s;
}

} // namespace

const std::vector<Language> & all() {
    static const std::vector<Language> table = [] {
        std::vector<Language> v;
        v.reserve(128);  // ~99 languages + auto; reserve prevents any realloc

        v.push_back({"auto", "Auto-detectar idioma"});

        const int n = whisper_lang_max_id();
        for (int i = 0; i <= n; ++i) {
            const char * code = whisper_lang_str(i);       // static internal pointer
            const char * full = whisper_lang_str_full(i);
            if (!code) continue;
            std::string label = full ? title_case(full) : std::string(code);
            label += " (";
            label += code;
            label += ")";
            v.push_back({code, std::move(label)});
        }
        return v;
    }();
    return table;
}

std::string name_for(const std::string & code) {
    for (const auto & item : all()) {
        if (code == item.code) return item.label;
    }
    return code;
}

} // namespace languages
