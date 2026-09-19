#include "harness.hpp"

#include "core/output_format.h"

#include <algorithm>

namespace {

inference::Result two_segment_result()
{
    inference::Result r;
    inference::Segment a;
    a.t0_ms = 0;
    a.t1_ms = 1500;
    a.text  = "Hola mundo";
    inference::Segment b;
    b.t0_ms = 1500;
    b.t1_ms = 3723;
    b.text  = "con \"comillas\", y coma";
    r.segments.push_back(a);
    r.segments.push_back(b);
    r.total_duration_ms = 3723;
    r.detected_language = "es";
    return r;
}

} // namespace

TEST(format_txt_two_segments)
{
    CHECK_EQ(inference::format_txt(two_segment_result()),
             std::string("Hola mundo\r\ncon \"comillas\", y coma\r\n"));
}

TEST(format_srt_two_segments)
{
    std::string expected =
        "1\r\n"
        "00:00:00,000 --> 00:00:01,500\r\n"
        "Hola mundo\r\n\r\n"
        "2\r\n"
        "00:00:01,500 --> 00:00:03,723\r\n"
        "con \"comillas\", y coma\r\n\r\n";
    CHECK_EQ(inference::format_srt(two_segment_result()), expected);
}

TEST(format_vtt_two_segments)
{
    std::string expected =
        "WEBVTT\r\n\r\n"
        "00:00:00.000 --> 00:00:01.500\r\n"
        "Hola mundo\r\n\r\n"
        "00:00:01.500 --> 00:00:03.723\r\n"
        "con \"comillas\", y coma\r\n\r\n";
    CHECK_EQ(inference::format_vtt(two_segment_result()), expected);
}

TEST(format_csv_escapes)
{
    std::string expected =
        "start_ms,end_ms,text\r\n"
        "0,1500,Hola mundo\r\n"
        "1500,3723,\"con \"\"comillas\"\", y coma\"\r\n";
    CHECK_EQ(inference::format_csv(two_segment_result()), expected);
}

TEST(format_json_escapes_and_language)
{
    std::string js = inference::format_json(two_segment_result());
    CHECK(js.find("\"detected_language\": \"es\"") != std::string::npos);
    CHECK(js.find("con \\\"comillas\\\", y coma") != std::string::npos);
    CHECK(js.find("\"t1_ms\": 3723") != std::string::npos);
}

TEST(format_json_marks_repetition_runs)
{
    inference::Result r = two_segment_result();
    r.segments[1].rep_run = true;      // duplicate of a repetition loop
    std::string js = inference::format_json(r);
    CHECK(js.find("\"repetition\": false") != std::string::npos);  // first stays clean
    CHECK(js.find("\"repetition\": true")  != std::string::npos);  // duplicate flagged
}

TEST(format_human_collapses_repetition_runs)
{
    inference::Result r = two_segment_result();
    // "Hola" x3 then the quoted line: period-1 run, first occurrence stays.
    inference::Segment dup1 = r.segments[0];
    inference::Segment dup2 = r.segments[0];
    dup1.rep_run = true;
    dup2.rep_run = true;
    r.segments.insert(r.segments.begin() + 1, dup2);
    r.segments.insert(r.segments.begin() + 1, dup1);

    CHECK_EQ(inference::format_txt(r),
             std::string("Hola mundo\r\ncon \"comillas\", y coma\r\n"));
    // SRT: duplicates removed and the next cue renumbered; exactly 2 cues left
    // (4 CRLF pairs each), so 8 bare newlines.
    std::string srt = inference::format_srt(r);
    CHECK_EQ(std::count(srt.begin(), srt.end(), '\n'), (long) 8);
    CHECK(srt.find("2\r\n00:00:01,500") != std::string::npos);
}

TEST(format_lrc_collapses_repetition_runs)
{
    inference::Result r = two_segment_result();
    inference::Segment dup1 = r.segments[0];
    inference::Segment dup2 = r.segments[0];
    dup1.rep_run = true;
    dup2.rep_run = true;
    r.segments.push_back(dup1);
    r.segments.push_back(dup2);

    std::string lrc = inference::format_lrc(r);
    CHECK(lrc.find("Hola mundo") != std::string::npos);
    // Only the first occurrence and the following line survive.
    CHECK_EQ(std::count(lrc.begin(), lrc.end(), '\n'), (long) 2);
}

TEST(format_json_full_tokens_and_settings)
{
    inference::Result r = two_segment_result();
    r.transcribe_ms     = 1234;
    r.confidence_overall = 0.91f;
    r.tier              = inference::ConfidenceTier::Good;

    inference::TokenInfo tok;
    tok.text     = " Hola";           // raw whisper token text keeps the leading space
    tok.id       = 123;
    tok.p        = 0.98f;
    tok.plog     = -0.02f;
    tok.t0_ms    = 0;
    tok.t1_ms    = 900;
    tok.vlen     = 0.5f;
    tok.is_special = false;

    inference::TokenInfo sp;          // a "special" token (id >= tok_eot)
    sp.text       = "[_SILENCE_]";
    sp.id         = 50258;
    sp.p          = 1.0f;
    sp.plog       = 0.0f;
    sp.t0_ms      = 900;
    sp.t1_ms      = 1500;
    sp.vlen       = 0.0f;
    sp.is_special = true;
    r.segments[0].tokens.push_back(tok);
    r.segments[0].tokens.push_back(sp);

    r.settings_used = cfg::Settings::fast_defaults().with_max_info_preset();

    std::string js = inference::format_json_full(r);
    CHECK(js.find("\"transcribe_ms\": 1234") != std::string::npos);
    CHECK(js.find("\"tier\": \"good\"") != std::string::npos);

    // settings snapshot (self-describing export)
    CHECK(js.find("\"settings\": {") != std::string::npos);
    CHECK(js.find("\"keep_tokens\": true") != std::string::npos);
    CHECK(js.find("\"suppress_nst\": false") != std::string::npos);
    CHECK(js.find("\"split_on_word\": true") != std::string::npos);

    // tokens: both a content token and a flagged special
    CHECK(js.find("\"tokens\": [") != std::string::npos);
    CHECK(js.find("\"text\": \" Hola\"") != std::string::npos);
    CHECK(js.find("\"id\": 123") != std::string::npos);
    CHECK(js.find("\"is_special\": false") != std::string::npos);
    CHECK(js.find("\"text\": \"[_SILENCE_]\"") != std::string::npos);
    CHECK(js.find("\"is_special\": true") != std::string::npos);

    // escaping from the shared path still applies
    CHECK(js.find("con \\\"comillas\\\", y coma") != std::string::npos);
}
