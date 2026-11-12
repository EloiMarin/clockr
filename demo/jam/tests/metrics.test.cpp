// jam metrics — deterministic, pure, bounded analysis of a score or a tick.

#include "jam/metrics.hpp"

#include "fixtures.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

namespace
{

jam::event_batch
sample_batch()
{
    jam::event_batch batch {};

    batch.events[0] = jam::note_event { 60, 100, 0, 0, true };
    batch.events[1] = jam::note_event { 64, 80, 0, 1, true };
    batch.events[2] = jam::note_event { 60, 100, 0, 2, true };
    batch.events[3] = jam::note_event { 60, 0, 0, 0, false };
    batch.count = 4;

    return batch;
}

void
require_metrics_equal(const jam::metrics& a, const jam::metrics& b)
{
    REQUIRE(a.event_density == b.event_density);
    REQUIRE(a.simultaneity == b.simultaneity);
    REQUIRE(a.register_spread == b.register_spread);
    REQUIRE(a.duplicate_notes == b.duplicate_notes);
    REQUIRE(a.unresolved_sustains == b.unresolved_sustains);
    REQUIRE(a.clipping_risk == b.clipping_risk);
    REQUIRE(a.voice_pressure == b.voice_pressure);
}

} // namespace

TEST_CASE("jam/metrics | Empty score is all zero", "[jam][metrics]")
{
    const auto m { jam::compute(jam::score {}) };

    REQUIRE(m.event_density == 0.0);
    REQUIRE(m.simultaneity == 0);
    REQUIRE(m.register_spread == 0);
    REQUIRE(m.duplicate_notes == 0);
    REQUIRE(m.unresolved_sustains == 0);
    REQUIRE(m.clipping_risk == 0.0);
    REQUIRE(m.voice_pressure == 0);
}

TEST_CASE(
    "jam/metrics | Score fields describe audible tracks",
    "[jam][metrics]"
)
{
    const auto one { jam::compute(jam_test::small_score()) };

    // Four active steps, two gated notes; one track only.
    REQUIRE(one.event_density == Catch::Approx(0.5));
    REQUIRE(one.simultaneity == 1);
    REQUIRE(one.register_spread == 4); // notes 60 and 64
    REQUIRE(one.duplicate_notes == 0);
    REQUIRE(one.unresolved_sustains == 0);
    REQUIRE(one.clipping_risk == Catch::Approx(100.0 / 127.0));
    REQUIRE(one.voice_pressure == 0);

    auto doubled { jam_test::small_score() };
    doubled.tracks[1] = jam::track { 0, 0, 255, false };

    const auto two { jam::compute(doubled) };

    // Both tracks play the same pattern: each column has two notes.
    REQUIRE(two.event_density == Catch::Approx(0.5));
    REQUIRE(two.simultaneity == 2);
    REQUIRE(two.register_spread == 4);
    REQUIRE(two.duplicate_notes == 2); // note 60 at col 0, note 64 at col 2
    REQUIRE(two.unresolved_sustains == 0);
    REQUIRE(two.clipping_risk == Catch::Approx(2.0 * 100.0 / 127.0));
    REQUIRE(two.voice_pressure == 0);
}

TEST_CASE("jam/metrics | Score metrics are deterministic", "[jam][metrics]")
{
    auto s { jam_test::small_score() };
    s.tracks[1] = jam::track { 0, 12, 180, false };
    s.patterns[0].steps[1] = jam::step { 62, 90, true };

    require_metrics_equal(jam::compute(s), jam::compute(s));
}

TEST_CASE(
    "jam/metrics | Added notes raise density and simultaneity",
    "[jam][metrics]"
)
{
    const auto base { jam::compute(jam_test::small_score()) };

    auto denser { jam_test::small_score() };
    denser.patterns[0].steps[1] = jam::step { 62, 80, true };

    const auto dense { jam::compute(denser) };

    REQUIRE(dense.event_density > base.event_density);

    auto wider { jam_test::small_score() };
    wider.tracks[1] = jam::track { 0, 0, 200, false };

    const auto wide { jam::compute(wider) };

    REQUIRE(wide.simultaneity > base.simultaneity);
    REQUIRE(wide.clipping_risk > base.clipping_risk);
}

TEST_CASE("jam/metrics | Event fields follow a rendered tick", "[jam][metrics]")
{
    jam::voice_state voices {};
    voices.slots[0].active = true;
    voices.slots[1].active = true;

    const auto m { jam::compute(sample_batch(), voices) };

    REQUIRE(m.event_density == Catch::Approx(3.0 / 64.0));
    REQUIRE(m.simultaneity == 3);
    REQUIRE(m.register_spread == 4);     // notes 60 and 64
    REQUIRE(m.duplicate_notes == 1);     // two note-60 onsets at offset 0
    REQUIRE(m.unresolved_sustains == 2); // tracks 1 and 2 have no off
    REQUIRE(m.clipping_risk == Catch::Approx(280.0 / 127.0));
    REQUIRE(m.voice_pressure == 2);
}

TEST_CASE("jam/metrics | Empty event batch is all zero", "[jam][metrics]")
{
    const auto m { jam::compute(jam::event_batch {}, jam::voice_state {}) };

    REQUIRE(m.event_density == 0.0);
    REQUIRE(m.simultaneity == 0);
    REQUIRE(m.register_spread == 0);
    REQUIRE(m.duplicate_notes == 0);
    REQUIRE(m.unresolved_sustains == 0);
    REQUIRE(m.clipping_risk == 0.0);
    REQUIRE(m.voice_pressure == 0);
}

TEST_CASE("jam/metrics | Event metrics are deterministic", "[jam][metrics]")
{
    jam::voice_state voices {};
    voices.slots[0].active = true;
    voices.slots[0].velocity = 100;

    require_metrics_equal(
        jam::compute(sample_batch(), voices),
        jam::compute(sample_batch(), voices)
    );
}

TEST_CASE(
    "jam/metrics | Added event raises density and pressure",
    "[jam][metrics]"
)
{
    const auto base_batch { sample_batch() };
    const auto base { jam::compute(base_batch, jam::voice_state {}) };

    auto denser { base_batch };
    denser.events[denser.count] = jam::note_event { 67, 90, 0, 3, true };
    denser.count = static_cast<std::uint8_t>(denser.count + 1);

    const auto dense { jam::compute(denser, jam::voice_state {}) };
    REQUIRE(dense.event_density > base.event_density);

    auto voices { jam::voice_state {} };
    voices.slots[0].active = true;

    const auto pressured { jam::compute(base_batch, voices) };
    REQUIRE(pressured.voice_pressure > base.voice_pressure);
}
