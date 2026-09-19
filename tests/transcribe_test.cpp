#include "harness.hpp"

#include "core/transcribe.h"

#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

constexpr int kSr = 16000;

// 16 kHz mono float: loud 440 Hz tone during the given [start, end] second
// intervals, silence elsewhere.
std::vector<float> tone_buffer(std::initializer_list<std::pair<double, double>> tones,
                               double total_s)
{
    std::vector<float> buf((size_t) (total_s * kSr), 0.0f);
    for (const auto & pr : tones) {
        const int64_t a = (int64_t) (pr.first * kSr);
        const int64_t b = (int64_t) (pr.second * kSr);
        for (int64_t i = a; i < b && i < (int64_t) buf.size(); ++i)
            buf[(size_t) i] = 0.5f * std::sin(2.0 * 3.14159265358979 * 440.0 * i / kSr);
    }
    return buf;
}

int64_t to_ms(int64_t samples) { return samples * 1000 / kSr; }

} // namespace

TEST(silence_trim_detects_two_spans_and_maps_times)
{
    // Speech at 1-2 s and 5-6 s, silence in between (gap 3 s > 2 s): the two
    // spans must NOT be merged.
    std::vector<float> s = tone_buffer({{1.0, 2.0}, {5.0, 6.0}}, 8.0);

    auto spans = inference::detect_speech_spans(s);
    CHECK_EQ(spans.size(), (size_t) 2);
    if (spans.size() != 2) return;

    // ~300 ms of padding is kept around each edge (sample-space bounds).
    CHECK(spans[0].begin <= 16000 * 700 / 1000);   // <= 0.70 s
    CHECK(spans[0].end   >= 16000 * 2000 / 1000);  // >= 2.00 s
    CHECK(spans[1].begin <= 16000 * 5000 / 1000 + 16000 * 300 / 1000);

    std::vector<float> clipped;
    inference::ClipMap m = inference::build_clip_map(s, spans, &clipped);
    CHECK(clipped.size() < s.size());
    CHECK_EQ(clipped.size(), (size_t) (spans[0].end - spans[0].begin) +
                                          (size_t) (spans[1].end - spans[1].begin));

    // Monotonic mapping: clipped time -> absolute original time; every point
    // stays inside [first absolute begin, last absolute end].
    const int64_t abs_begin = to_ms(spans[0].begin);
    const int64_t abs_end   = to_ms(spans[1].end);
    const int64_t total_ms  = to_ms((int64_t) clipped.size());
    int64_t prev = -1;
    for (int64_t ms = 0; ms <= total_ms + 50; ms += 50) {
        const int64_t v = m.map_ms(ms);
        CHECK(v >= prev);
        CHECK(v >= abs_begin && v <= abs_end);
        prev = v;
    }
    // Exact interior identity: 500 ms into the second kept span lands 500 ms
    // past its absolute start (16 samples per ms at 16 kHz).
    const int64_t first_len_ms = to_ms(spans[0].end - spans[0].begin);
    CHECK_EQ(m.map_ms(first_len_ms + 500), to_ms(spans[1].begin + 500 * 16));
}

TEST(silence_trim_merges_short_gaps)
{
    // Speech at 1-2 s and 2.5-3.5 s: the 0.5 s gap is below min_silence_ms.
    std::vector<float> s = tone_buffer({{1.0, 2.0}, {2.5, 3.5}}, 4.5);

    auto spans = inference::detect_speech_spans(s);
    CHECK_EQ(spans.size(), (size_t) 1);
    if (spans.size() != 1) return;
    // One merged span covering from ~0.7 s (padded) through ~3.8 s (padded).
    CHECK(spans[0].begin <= 16000 * 700 / 1000);    // <= 0.70 s
    CHECK(spans[0].end   >= 16000 * 3800 / 1000);   // >= 3.80 s
}

TEST(silence_trim_all_silence_yields_empty)
{
    std::vector<float> s(16000 * 4, 0.0f);
    CHECK(inference::detect_speech_spans(s).empty());
}

TEST(silence_trim_quiet_audio_below_threshold_yields_empty)
{
    // RMS ~0.00035 << 0.004 threshold (room-tone level).
    std::vector<float> s(16000 * 4, 0.0f);
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = 0.0005f * std::sin(2.0 * 3.14159265 * 200.0 * i / 16000);
    CHECK(inference::detect_speech_spans(s).empty());
}

TEST(repetition_run_period1_flags_duplicates)
{
    inference::Segment a; a.text = "una frase";
    inference::Segment b; b.text = "una frase";
    inference::Segment c; c.text = "una frase";
    inference::Segment d; d.text = "otra cosa";
    std::vector<inference::Segment> segs{a, b, c, d};
    inference::mark_repetition_runs(segs);
    CHECK(!segs[0].rep_run);   // first occurrence stays clean
    CHECK(segs[1].rep_run);
    CHECK(segs[2].rep_run);
    CHECK(!segs[3].rep_run);
}

TEST(repetition_run_period2_flags_alternating_loop)
{
    // The real-world shape: "Y… / si es que se tratara de un lugar." x3.
    inference::Segment y1; y1.text = "Y...";
    inference::Segment s1; s1.text = "si es que se tratara de un lugar.";
    inference::Segment y2; y2.text = "Y...";
    inference::Segment s2; s2.text = "si es que se tratara de un lugar.";
    inference::Segment y3; y3.text = "Y...";
    inference::Segment s3; s3.text = "si es que se tratara de un lugar.";
    inference::Segment x;  x.text  = "de vuelta al tema";
    std::vector<inference::Segment> segs{y1, s1, y2, s2, y3, s3, x};
    inference::mark_repetition_runs(segs);
    CHECK(!segs[0].rep_run);
    CHECK(!segs[1].rep_run);   // first full period stays clean
    CHECK(segs[2].rep_run);
    CHECK(segs[3].rep_run);
    CHECK(segs[4].rep_run);
    CHECK(segs[5].rep_run);
    CHECK(!segs[6].rep_run);
}

TEST(repetition_run_two_identical_not_flagged)
{
    inference::Segment a; a.text = "misma";
    inference::Segment b; b.text = "misma";
    inference::Segment c; c.text = "otra";
    std::vector<inference::Segment> segs{a, b, c};
    inference::mark_repetition_runs(segs);
    CHECK(!segs[0].rep_run);
    CHECK(!segs[1].rep_run);
    CHECK(!segs[2].rep_run);
}

TEST(repetition_run_period2_not_enough_cycles_not_flagged)
{
    // ABAB (2 cycles) is below the 6-segment guard: conservative on purpose.
    inference::Segment a1; a1.text = "A";
    inference::Segment b1; b1.text = "B";
    inference::Segment a2; a2.text = "A";
    inference::Segment b2; b2.text = "B";
    std::vector<inference::Segment> segs{a1, b1, a2, b2};
    inference::mark_repetition_runs(segs);
    for (const auto & sg : segs) CHECK(!sg.rep_run);
}

TEST(span_groups_pack_short_spans_into_one_unit)
{
    // 1 s + 1 s + 0.625 s of audio fit far under the 90 s budget -> one unit
    // covering the whole kept span, starting at offset 0.
    std::vector<inference::SpeechSpan> spans{{0, 16000}, {16000, 32000}, {100000, 110000}};
    auto units = inference::build_call_units(spans, 90 * 16000);
    CHECK_EQ(units.size(), (size_t) 1);
    if (units.size() != 1) return;
    CHECK_EQ(units[0].clip_offset, (int64_t) 0);
    CHECK_EQ(units[0].clip_len, (int64_t) (16000 + 16000 + 10000));
}

TEST(span_groups_flush_when_next_span_does_not_fit)
{
    // 60 s + 60 s with a 90 s budget: the second span cannot join the first
    // unit, so two units of 60 s each, contiguous.
    std::vector<inference::SpeechSpan> spans{{0, 60 * 16000}, {60 * 16000, 120 * 16000}};
    auto units = inference::build_call_units(spans, 90 * 16000);
    CHECK_EQ(units.size(), (size_t) 2);
    if (units.size() != 2) return;
    CHECK_EQ(units[0].clip_offset, (int64_t) 0);
    CHECK_EQ(units[0].clip_len, (int64_t) (60 * 16000));
    CHECK_EQ(units[1].clip_offset, (int64_t) (60 * 16000));
    CHECK_EQ(units[1].clip_len, (int64_t) (60 * 16000));
}

TEST(span_groups_slice_oversized_span)
{
    // A single 200 s span with a 90 s budget -> three slices (90 + 90 + 20).
    std::vector<inference::SpeechSpan> spans{{0, 200 * 16000}};
    auto units = inference::build_call_units(spans, 90 * 16000);
    CHECK_EQ(units.size(), (size_t) 3);
    if (units.size() != 3) return;
    CHECK_EQ(units[0].clip_offset, (int64_t) 0);
    CHECK_EQ(units[0].clip_len, (int64_t) (90 * 16000));
    CHECK_EQ(units[1].clip_offset, (int64_t) (90 * 16000));
    CHECK_EQ(units[1].clip_len, (int64_t) (90 * 16000));
    CHECK_EQ(units[2].clip_offset, (int64_t) (180 * 16000));
    CHECK_EQ(units[2].clip_len, (int64_t) (20 * 16000));
}

TEST(span_groups_tile_kept_audio_exactly)
{
    // Mixed span sizes (including a giant one and a tiny one) with a small
    // budget: units must partition the kept audio with no gaps/overlaps, in
    // order, each within budget.
    std::vector<inference::SpeechSpan> spans{{0, 10 * 16000}, {100 * 16000, 130 * 16000},
                                             {300 * 16000, 305 * 16000}, {500 * 16000, 600 * 16000}};
    const int64_t budget = 25 * 16000;
    auto units = inference::build_call_units(spans, budget);
    CHECK(!units.empty());
    int64_t expect = 0, total = 0;
    for (const auto & s : spans) total += s.end - s.begin;
    for (const auto & u : units) {
        CHECK(u.clip_len > 0);
        CHECK(u.clip_len <= budget);
        CHECK_EQ(u.clip_offset, expect);
        expect += u.clip_len;
    }
    CHECK_EQ(expect, total);
}

TEST(span_groups_empty_input_yields_no_units)
{
    CHECK(inference::build_call_units({}, 90 * 16000).empty());
}