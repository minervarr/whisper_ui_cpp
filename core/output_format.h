#pragma once

#include <string>

#include "core/transcribe.h"

namespace inference {

// Each function returns the full file content in UTF-8, ready to write to
// disk. Canonical extensions are the function names.

std::string format_txt(const Result & r);   // plain text, one line per segment, no timestamps
std::string format_vtt(const Result & r);   // WebVTT, HH:MM:SS.mmm
std::string format_srt(const Result & r);   // SRT, HH:MM:SS,mmm
std::string format_json(const Result & r);  // full structure with segments + confidence metrics
std::string format_lrc(const Result & r);   // lyrics with [MM:SS.cc] timestamps
std::string format_csv(const Result & r);   // start_ms,end_ms,text (escaped CSV)
std::string format_tsv(const Result & r);   // start_ms<TAB>end_ms<TAB>text

} // namespace inference
