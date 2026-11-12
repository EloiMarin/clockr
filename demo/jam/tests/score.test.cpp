// jam score/patch — pure data and pure edits. No engine, no clocks.

#include "jam/patch.hpp"
#include "jam/score.hpp"

#include "fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

using namespace jam;
using jam_test::make_patch;
using jam_test::small_score;

TEST_CASE("jam/score | set_step writes a step and extends the pattern", "[jam]")
{
    const auto s { small_score() };

    const auto out { apply(s, make_patch({
        patch_step { patch_op::set_step, 0, 1, 67, 90 },
    })) };

    // The input is untouched (pure).
    REQUIRE(s.patterns[0].steps[1].gate == false);

    REQUIRE(out.patterns[0].steps[1].note == 67);
    REQUIRE(out.patterns[0].steps[1].velocity == 90);
    REQUIRE(out.patterns[0].steps[1].gate);
    REQUIRE(out.patterns[0].length == 4);

    // Writing past the active length extends it (up to 32).
    const auto extended { apply(s, make_patch({
        patch_step { patch_op::set_step, 0, 7, 72, 100 },
    })) };

    REQUIRE(extended.patterns[0].length == 8);
    REQUIRE(extended.patterns[0].steps[7].note == 72);
}

TEST_CASE("jam/score | clear_step silences a step without shrinking", "[jam]")
{
    const auto s { small_score() };

    const auto out { apply(s, make_patch({
        patch_step { patch_op::clear_step, 0, 0, 0, 0 },
    })) };

    REQUIRE(!out.patterns[0].steps[0].gate);
    REQUIRE(out.patterns[0].steps[0].velocity == 0);
    REQUIRE(out.patterns[0].length == 4);
    REQUIRE(out.patterns[0].steps[2].gate);
}

TEST_CASE("jam/score | transpose shifts gated notes and clamps to MIDI", "[jam]")
{
    const auto s { small_score() };

    const auto up { apply(s, make_patch({
        patch_step { patch_op::transpose, 0, 0, 0, 12 },
    })) };

    REQUIRE(up.patterns[0].steps[0].note == 72);
    REQUIRE(up.patterns[0].steps[2].note == 76);

    const auto high { apply(s, make_patch({
        patch_step { patch_op::transpose, 0, 0, 0, 100 },
    })) };

    REQUIRE(high.patterns[0].steps[0].note == 127);
    REQUIRE(high.patterns[0].steps[2].note == 127);
}

TEST_CASE("jam/score | rotate moves the active length in either direction", "[jam]")
{
    const auto s { small_score() };

    const auto right { apply(s, make_patch({
        patch_step { patch_op::rotate, 0, 0, 0, 1 },
    })) };

    REQUIRE(right.patterns[0].steps[1].note == 60);
    REQUIRE(right.patterns[0].steps[3].note == 64);
    REQUIRE(!right.patterns[0].steps[0].gate);

    const auto left { apply(s, make_patch({
        patch_step { patch_op::rotate, 0, 0, 0, -1 },
    })) };

    REQUIRE(left.patterns[0].steps[3].note == 60);
    REQUIRE(left.patterns[0].steps[1].note == 64);
}

TEST_CASE("jam/score | mask keeps only the steps selected by its byte", "[jam]")
{
    const auto s { small_score() };

    const auto keep { apply(s, make_patch({
        patch_step { patch_op::mask, 0, 0x05, 0, 0 },
    })) };

    REQUIRE(keep.patterns[0].steps[0].gate);
    REQUIRE(keep.patterns[0].steps[2].gate);

    const auto thinned { apply(s, make_patch({
        patch_step { patch_op::mask, 0, 0x01, 0, 0 },
    })) };

    REQUIRE(thinned.patterns[0].steps[0].gate);
    REQUIRE(!thinned.patterns[0].steps[2].gate);
}

TEST_CASE("jam/score | density keeps b of every c positions", "[jam]")
{
    auto s { small_score() };
    s.patterns[0].steps[1] = step { 62, 90, true };
    s.patterns[0].steps[3] = step { 65, 90, true };

    const auto half { apply(s, make_patch({
        patch_step { patch_op::density, 0, 1, 2, 0 },
    })) };

    REQUIRE(half.patterns[0].steps[0].gate);
    REQUIRE(!half.patterns[0].steps[1].gate);
    REQUIRE(half.patterns[0].steps[2].gate);
    REQUIRE(!half.patterns[0].steps[3].gate);

    // b >= c keeps everything; c == 0 is a no-op.
    const auto all { apply(s, make_patch({
        patch_step { patch_op::density, 0, 9, 2, 0 },
    })) };

    REQUIRE(all == s);

    const auto untouched { apply(s, make_patch({
        patch_step { patch_op::density, 0, 1, 0, 0 },
    })) };

    REQUIRE(untouched == s);
}

TEST_CASE("jam/score | stack fills silent steps and extends to the longest", "[jam]")
{
    auto s { small_score() };
    s.patterns[1].length = 3;
    s.patterns[1].steps[1] = step { 67, 70, true };

    const auto out { apply(s, make_patch({
        patch_step { patch_op::stack, 0, 1, 0, 0 },
    })) };

    REQUIRE(out.patterns[0].length == 4);
    REQUIRE(out.patterns[0].steps[0].note == 60); // destination wins
    REQUIRE(out.patterns[0].steps[1].note == 67); // source fills the gap
    REQUIRE(out.patterns[0].steps[2].note == 64);
}

TEST_CASE("jam/score | sequence appends source steps and truncates at 32", "[jam]")
{
    auto s { small_score() };
    s.patterns[1].length = 3;
    s.patterns[1].steps[0] = step { 70, 60, true };
    s.patterns[1].steps[1] = step { 71, 60, true };
    s.patterns[1].steps[2] = step { 72, 60, true };

    const auto out { apply(s, make_patch({
        patch_step { patch_op::sequence, 0, 1, 0, 0 },
    })) };

    REQUIRE(out.patterns[0].length == 7);
    REQUIRE(out.patterns[0].steps[4].note == 70);
    REQUIRE(out.patterns[0].steps[6].note == 72);

    s.patterns[1].length = 32;
    for (std::size_t i { 0 }; i < steps_per_pattern; ++i)
        s.patterns[1].steps[i] = step { 80, 60, true };

    const auto clipped { apply(s, make_patch({
        patch_step { patch_op::sequence, 0, 1, 0, 0 },
    })) };

    REQUIRE(clipped.patterns[0].length == 32);
    REQUIRE(clipped.patterns[0].steps[31].note == 80);
}

TEST_CASE("jam/score | operations compose in list order", "[jam]")
{
    const auto s { small_score() };

    const auto written_then_transposed { apply(s, make_patch({
        patch_step { patch_op::set_step, 0, 1, 60, 100 },
        patch_step { patch_op::transpose, 0, 0, 0, 12 },
    })) };

    REQUIRE(written_then_transposed.patterns[0].steps[1].note == 72);

    const auto transposed_then_written { apply(s, make_patch({
        patch_step { patch_op::transpose, 0, 0, 0, 12 },
        patch_step { patch_op::set_step, 0, 1, 60, 100 },
    })) };

    REQUIRE(transposed_then_written.patterns[0].steps[1].note == 60);
}

TEST_CASE("jam/score | invalid indices, ops and counts are no-ops", "[jam]")
{
    const auto s { small_score() };

    const auto invalid_pattern { apply(s, make_patch({
        patch_step { patch_op::set_step, 200, 0, 60, 100 },
        patch_step { patch_op::clear_step, 99, 0, 0, 0 },
        patch_step { patch_op::transpose, 42, 0, 0, 12 },
        patch_step { patch_op::rotate, 16, 0, 0, 1 },
        patch_step { patch_op::mask, 16, 0, 0, 0 },
        patch_step { patch_op::density, 16, 1, 2, 0 },
        patch_step { patch_op::stack, 0, 200, 0, 0 },
        patch_step { patch_op::sequence, 0, 200, 0, 0 },
    })) };

    REQUIRE(invalid_pattern == s);

    const auto invalid_step { apply(s, make_patch({
        patch_step { patch_op::set_step, 0, 200, 60, 100 },
        patch_step { patch_op::clear_step, 0, 32, 0, 0 },
    })) };

    REQUIRE(invalid_step == s);

    const auto unknown_op { apply(s, make_patch({
        patch_step { static_cast<patch_op>(250), 0, 0, 0, 0 },
    })) };

    REQUIRE(unknown_op == s);

    // count larger than the array is clamped, never read past the end.
    auto oversized { make_patch({}) };
    oversized.count = 200;
    oversized.ops[0] = patch_step { patch_op::set_step, 0, 1, 67, 90 };

    const auto clamped { apply(s, oversized) };
    REQUIRE(clamped.patterns[0].steps[1].note == 67);
    REQUIRE(clamped.patterns[0].steps[2].note == 64);
}

TEST_CASE("jam/score | Golden patch produces the expected score", "[jam]")
{
    const auto s { small_score() };

    const auto out { apply(s, make_patch({
        patch_step { patch_op::set_step, 0, 1, 62, 70 },
        patch_step { patch_op::rotate, 0, 0, 0, 2 },
        patch_step { patch_op::transpose, 0, 0, 0, 5 },
        patch_step { patch_op::mask, 0, 0x0F, 0, 0 },
        patch_step { patch_op::density, 0, 3, 4, 0 },
    })) };

    score expected { small_score() };
    expected.patterns[0].steps[0] = step { 69, 80, true };
    expected.patterns[0].steps[1] = step { 60, 0, false };
    expected.patterns[0].steps[2] = step { 65, 100, true };
    expected.patterns[0].steps[3] = step { 60, 0, false };

    REQUIRE(out == expected);
}
