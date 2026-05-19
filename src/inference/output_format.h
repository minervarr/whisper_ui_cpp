#pragma once

#include <string>

#include "transcribe.h"

namespace inference {

// Cada función devuelve el contenido completo en UTF-8 listo para escribir a disco.
// Las extensiones canónicas son las del nombre.

std::string format_txt(const Result & r);   // texto plano, una linea por segmento, sin timestamps
std::string format_vtt(const Result & r);   // WebVTT, HH:MM:SS.mmm
std::string format_srt(const Result & r);   // SRT, HH:MM:SS,mmm
std::string format_json(const Result & r);  // estructura completa con segmentos y métricas de confianza
std::string format_lrc(const Result & r);   // letra con timestamps [MM:SS.cc]
std::string format_csv(const Result & r);   // start_ms,end_ms,text (CSV con escape)
std::string format_tsv(const Result & r);   // start_ms<TAB>end_ms<TAB>text

} // namespace inference
