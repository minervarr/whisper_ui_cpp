#include "core/output_format.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace inference {

namespace {

void format_hms_ms(int64_t ms, char sep_ms, char * out, size_t out_sz) {
    int64_t total = ms;
    if (total < 0) total = 0;
    int64_t h  = total / 3600000;       total %= 3600000;
    int64_t m  = total / 60000;          total %= 60000;
    int64_t s  = total / 1000;           total %= 1000;
    snprintf(out, out_sz, "%02lld:%02lld:%02lld%c%03lld",
             (long long) h, (long long) m, (long long) s, sep_ms, (long long) total);
}

std::string format_vtt_time(int64_t ms) {
    char b[32]; format_hms_ms(ms, '.', b, sizeof(b)); return b;
}

std::string format_srt_time(int64_t ms) {
    char b[32]; format_hms_ms(ms, ',', b, sizeof(b)); return b;
}

std::string format_lrc_time(int64_t ms) {
    int64_t total = ms < 0 ? 0 : ms;
    int64_t mm = total / 60000;
    int64_t ss = (total % 60000) / 1000;
    int64_t cc = (total % 1000) / 10;  // centiseconds
    char b[16];
    snprintf(b, sizeof(b), "%02lld:%02lld.%02lld",
             (long long) mm, (long long) ss, (long long) cc);
    return b;
}

std::string json_escape(const std::string & in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = (unsigned char) in[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char) c;
                }
        }
    }
    return out;
}

std::string csv_escape(const std::string & in) {
    bool needs_quote = in.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quote) return in;
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (char c : in) {
        if (c == '"') out += "\"\"";
        else          out += c;
    }
    out.push_back('"');
    return out;
}

} // namespace

std::string format_txt(const Result & r) {
    std::string out;
    for (size_t i = 0; i < r.segments.size(); ++i) {
        if (r.segments[i].rep_run) continue;   // collapse repetition-loop duplicates
        out += r.segments[i].text;
        out += "\r\n";
    }
    return out;
}

std::string format_vtt(const Result & r) {
    std::string out = "WEBVTT\r\n\r\n";
    for (const auto & seg : r.segments) {
        if (seg.rep_run) continue;
        out += format_vtt_time(seg.t0_ms);
        out += " --> ";
        out += format_vtt_time(seg.t1_ms);
        out += "\r\n";
        out += seg.text;
        out += "\r\n\r\n";
    }
    return out;
}

std::string format_srt(const Result & r) {
    std::string out;
    size_t cue = 0;
    for (size_t i = 0; i < r.segments.size(); ++i) {
        if (r.segments[i].rep_run) continue;   // duplicates renumbered away
        char idx[16];
        snprintf(idx, sizeof(idx), "%zu\r\n", ++cue);
        out += idx;
        out += format_srt_time(r.segments[i].t0_ms);
        out += " --> ";
        out += format_srt_time(r.segments[i].t1_ms);
        out += "\r\n";
        out += r.segments[i].text;
        out += "\r\n\r\n";
    }
    return out;
}

std::string format_lrc(const Result & r) {
    std::string out;
    for (const auto & seg : r.segments) {
        if (seg.rep_run) continue;
        out += "[";
        out += format_lrc_time(seg.t0_ms);
        out += "]";
        out += seg.text;
        out += "\r\n";
    }
    return out;
}

std::string format_csv(const Result & r) {
    std::string out = "start_ms,end_ms,text\r\n";
    for (const auto & seg : r.segments) {
        char times[64];
        snprintf(times, sizeof(times), "%lld,%lld,",
                 (long long) seg.t0_ms, (long long) seg.t1_ms);
        out += times;
        out += csv_escape(seg.text);
        out += "\r\n";
    }
    return out;
}

std::string format_tsv(const Result & r) {
    std::string out = "start_ms\tend_ms\ttext\r\n";
    for (const auto & seg : r.segments) {
        char times[64];
        snprintf(times, sizeof(times), "%lld\t%lld\t",
                 (long long) seg.t0_ms, (long long) seg.t1_ms);
        out += times;
        std::string text = seg.text;
        // Tabs and newlines in the text break the format -> replace with spaces.
        for (char & c : text) {
            if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        }
        out += text;
        out += "\r\n";
    }
    return out;
}

std::string format_json(const Result & r) {
    std::ostringstream os;
    os << "{\r\n";
    os << "  \"detected_language\": \"" << json_escape(r.detected_language) << "\",\r\n";
    os << "  \"total_duration_ms\": " << r.total_duration_ms << ",\r\n";
    os.precision(4);
    os << std::fixed;
    os << "  \"confidence_overall\": " << r.confidence_overall << ",\r\n";
    os << "  \"tier\": \"";
    switch (r.tier) {
        case ConfidenceTier::Excellent: os << "excellent"; break;
        case ConfidenceTier::Good:      os << "good";      break;
        case ConfidenceTier::Low:       os << "low";       break;
    }
    os << "\",\r\n";
    os << "  \"segments\": [\r\n";
    for (size_t i = 0; i < r.segments.size(); ++i) {
        const auto & s = r.segments[i];
        os << "    {\r\n";
        os << "      \"t0_ms\": " << s.t0_ms << ",\r\n";
        os << "      \"t1_ms\": " << s.t1_ms << ",\r\n";
        os << "      \"text\": \"" << json_escape(s.text) << "\",\r\n";
        os << "      \"no_speech_prob\": " << s.no_speech_prob << ",\r\n";
        os << "      \"mean_token_p\": "   << s.mean_token_p   << ",\r\n";
        os << "      \"min_token_p\": "    << s.min_token_p    << ",\r\n";
        os << "      \"confidence\": "     << s.confidence     << ",\r\n";
        os << "      \"repetition\": "     << (s.rep_run ? "true" : "false") << "\r\n";
        os << "    }" << (i + 1 < r.segments.size() ? "," : "") << "\r\n";
    }
    os << "  ]\r\n";
    os << "}\r\n";
    return os.str();
}

std::string format_json_full(const Result & r) {
    std::ostringstream os;
    os << "{\r\n";
    os << "  \"detected_language\": \"" << json_escape(r.detected_language) << "\",\r\n";
    os << "  \"total_duration_ms\": " << r.total_duration_ms << ",\r\n";
    os << "  \"transcribe_ms\": " << r.transcribe_ms << ",\r\n";
    os.precision(4);
    os << std::fixed;
    os << "  \"confidence_overall\": " << r.confidence_overall << ",\r\n";
    os << "  \"tier\": \"";
    switch (r.tier) {
        case ConfidenceTier::Excellent: os << "excellent"; break;
        case ConfidenceTier::Good:      os << "good";      break;
        case ConfidenceTier::Low:       os << "low";       break;
    }
    os << "\",\r\n";

    // Snapshot of the settings that produced this result.
    const auto & s = r.settings_used;
    os << "  \"settings\": {\r\n";
    os << "    \"language\": \""          << json_escape(s.language)          << "\",\r\n";
    os << "    \"translate\": "           << (s.translate ? "true" : "false") << ",\r\n";
    os << "    \"detect_language\": "     << (s.detect_language ? "true" : "false") << ",\r\n";
    os << "    \"n_threads\": "           << s.n_threads << ",\r\n";
    os << "    \"use_beam_search\": "     << (s.use_beam_search ? "true" : "false") << ",\r\n";
    os << "    \"beam_size\": "           << s.beam_size << ",\r\n";
    os << "    \"best_of\": "             << s.best_of << ",\r\n";
    os << "    \"temperature\": "         << s.temperature << ",\r\n";
    os << "    \"temperature_inc\": "     << s.temperature_inc << ",\r\n";
    os << "    \"suppress_blank\": "      << (s.suppress_blank ? "true" : "false") << ",\r\n";
    os << "    \"suppress_nst\": "        << (s.suppress_nst ? "true" : "false") << ",\r\n";
    os << "    \"single_segment\": "      << (s.single_segment ? "true" : "false") << ",\r\n";
    os << "    \"split_on_word\": "       << (s.split_on_word ? "true" : "false") << ",\r\n";
    os << "    \"silence_trim\": "        << (s.silence_trim ? "true" : "false") << ",\r\n";
    os << "    \"keep_tokens\": "         << (s.keep_tokens ? "true" : "false") << ",\r\n";
    os << "    \"n_max_text_ctx\": "      << s.n_max_text_ctx << ",\r\n";
    os << "    \"max_len\": "             << s.max_len << ",\r\n";
    os << "    \"max_tokens\": "          << s.max_tokens << ",\r\n";
    os << "    \"audio_ctx\": "           << s.audio_ctx << ",\r\n";
    os << "    \"tdrz_enable\": "         << (s.tdrz_enable ? "true" : "false") << ",\r\n";
    os << "    \"carry_initial_prompt\": " << (s.carry_initial_prompt ? "true" : "false") << "\r\n";
    os << "  },\r\n";

    os << "  \"segments\": [\r\n";
    for (size_t i = 0; i < r.segments.size(); ++i) {
        const auto & seg = r.segments[i];
        os << "    {\r\n";
        os << "      \"t0_ms\": " << seg.t0_ms << ",\r\n";
        os << "      \"t1_ms\": " << seg.t1_ms << ",\r\n";
        os << "      \"text\": \"" << json_escape(seg.text) << "\",\r\n";
        os << "      \"no_speech_prob\": " << seg.no_speech_prob << ",\r\n";
        os << "      \"mean_token_p\": "   << seg.mean_token_p   << ",\r\n";
        os << "      \"min_token_p\": "    << seg.min_token_p    << ",\r\n";
        os << "      \"confidence\": "     << seg.confidence     << ",\r\n";
        os << "      \"repetition\": "     << (seg.rep_run ? "true" : "false") << ",\r\n";
        os << "      \"tokens\": [\r\n";
        for (size_t t = 0; t < seg.tokens.size(); ++t) {
            const auto & tok = seg.tokens[t];
            os << "        {\r\n";
            os << "          \"text\": \""      << json_escape(tok.text)      << "\",\r\n";
            os << "          \"id\": "          << tok.id << ",\r\n";
            os << "          \"p\": "           << tok.p << ",\r\n";
            os << "          \"plog\": "        << tok.plog << ",\r\n";
            os << "          \"t0_ms\": "       << tok.t0_ms << ",\r\n";
            os << "          \"t1_ms\": "       << tok.t1_ms << ",\r\n";
            os << "          \"vlen\": "        << tok.vlen << ",\r\n";
            os << "          \"is_special\": "  << (tok.is_special ? "true" : "false") << "\r\n";
            os << "        }" << (t + 1 < seg.tokens.size() ? "," : "") << "\r\n";
        }
        os << "      ]\r\n";
        os << "    }" << (i + 1 < r.segments.size() ? "," : "") << "\r\n";
    }
    os << "  ]\r\n";
    os << "}\r\n";
    return os.str();
}

} // namespace inference
