#include "harness.hpp"

#include "core/output_format.h"

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
