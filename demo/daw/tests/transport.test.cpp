// daw transport math — pure-function tests, zero engine work (§6.4).
//
// Each row pins one musical edge case against daw::transport_step (the
// executable spec) or the individual step functions it composes.

#include "daw/transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <optional>

using namespace daw;

namespace
{

// One-beat blocks at 120 bpm: the fixture every spec test below steps.
constexpr TransportIn tick_in(bool playing = true,
                              std::optional<double> seek = std::nullopt,
                              double ls = 0.0,
                              double le = 4.0,
                              bool on = true)
{
    return TransportIn { BlockDesc { 0, 24000, 48000.0 }, LoopRegion { ls, le, on }, playing, seek, 120.0 };
}

} // namespace

TEST_CASE("daw/transport | Block advance is exact at constant tempo", "[daw]")
{
    static_assert(beats_per_block(24000, 48000.0, 120.0) == 1.0);

    REQUIRE(beats_per_block(24000, 48000.0, 120.0) == 1.0);
    REQUIRE(beats_per_block(512, 48000.0, 120.0) == Catch::Approx(512.0 * 120.0 / (60.0 * 48000.0)));
    REQUIRE(beats_per_block(48000, 48000.0, 60.0) == Catch::Approx(1.0));
}

TEST_CASE("daw/transport | Pause gates the advance, seek wins over it", "[daw]")
{
    static_assert(transport_step(0.0, tick_in()) == 1.0);
    static_assert(transport_step(5.0, tick_in(false)) == 5.0);
    static_assert(transport_step(5.0, tick_in(true, 2.5)) == 2.5);
    static_assert(transport_step(5.0, tick_in(false, 2.5)) == 2.5);

    // Spec, one tick from zero: playing advances, paused holds, seek jumps.
    REQUIRE(transport_step(0.0, tick_in()) == Catch::Approx(1.0));
    REQUIRE(transport_step(5.0, tick_in(false)) == Catch::Approx(5.0));
    REQUIRE(transport_step(5.0, tick_in(true, 2.5)) == Catch::Approx(2.5));
    REQUIRE(transport_step(5.0, tick_in(false, 2.5)) == Catch::Approx(2.5));
}

TEST_CASE("daw/transport | Loop wraps with remainder, not clamp", "[daw]")
{
    static_assert(wrap_pos(4.0, 0.0, 4.0, true) == 0.0);
    static_assert(wrap_pos(4.5, 0.0, 4.0, true) == 0.5);
    static_assert(wrap_pos(9.0, 1.0, 3.0, true) == 1.0);
    static_assert(wrap_pos(9.0, 0.0, 4.0, false) == 9.0);
    static_assert(wrap_pos(9.0, 2.0, 2.0, true) == 9.0);

    // Through the spec: five 1-beat blocks from 0 across a (0,4] loop.
    double pos { 0.0 };

    for (int i { 0 }; i < 5; ++i)
        pos = transport_step(pos, tick_in());

    REQUIRE(pos == Catch::Approx(1.0)); // 0,1,2,3,0,1 — remainder, never clamped
}

TEST_CASE("daw/transport | Seek past the end wraps once with loop on", "[daw]")
{
    // §6.4 pinned policy: loop on wraps once, off holds the landing.
    REQUIRE(transport_step(9.0, tick_in(true, 16.0)) == Catch::Approx(0.0));
    REQUIRE(transport_step(9.0, tick_in(true, 16.0, 0.0, 4.0, false)) == Catch::Approx(16.0));
}

TEST_CASE("daw/transport | Resume after seek advances from the landing", "[daw]")
{
    // Seek to 2.5 paused, resume two blocks: the scan carries the landing.
    double pos { transport_step(0.0, tick_in(false, 2.5)) };
    REQUIRE(pos == Catch::Approx(2.5));

    pos = transport_step(pos, tick_in());
    REQUIRE(pos == Catch::Approx(3.5));

    pos = transport_step(pos, tick_in());
    REQUIRE(pos == Catch::Approx(0.5)); // 4.5 wraps with remainder
}

TEST_CASE("daw/transport | Block-spanning loop end splits the firing range", "[daw]")
{
    // Note at 3.5, block (3,4] fires; (0,1] of the next lap does not.
    REQUIRE(schedule_count(3.0, 4.0, 0.0, 4.0, true, 3.5) == 1);
    REQUIRE(schedule_count(0.0, 1.0, 0.0, 4.0, true, 3.5) == 0);

    // Boundary convention: the interval is half-open — a note exactly on the
    // block start fired on the previous block, never twice.
    REQUIRE(schedule_count(3.0, 4.0, 0.0, 4.0, true, 3.0) == 0);
    REQUIRE(schedule_count(3.0, 3.5, 0.0, 4.0, true, 3.5) == 1);

    // Wrapped block (0.5 prev, 0.5 cur across the seam): tail range catches a
    // note at 3.5, head range a note at 0.5.
    REQUIRE(schedule_count(3.5, 0.5, 0.0, 4.0, true, 3.5) == 0); // (3.5,4] excludes 3.5
    REQUIRE(schedule_count(3.0, 0.5, 0.0, 4.0, true, 3.5) == 1);
    REQUIRE(schedule_count(3.0, 0.5, 0.0, 4.0, true, 0.5) == 1);
}

TEST_CASE("daw/transport | Sample offsets are exact under constant tempo", "[daw]")
{
    // Beat 3.5 half a beat into a block starting at beat 3: 12000 samples.
    REQUIRE(schedule_offset(3.0, 4.0, 0.0, 4.0, true, 72000, 24000, 48000.0, 120.0, 3.5) == 12000);
    REQUIRE(schedule_offset(0.0, 1.0, 0.0, 4.0, true, 0, 24000, 48000.0, 120.0, 3.5) == -1);

    // Defensive clamp: an inconsistent tempo maps past the block end.
    REQUIRE(schedule_offset(0.0, 1.0, 0.0, 4.0, false, 0, 512, 48000.0, 960.0, 1.0) == 511);
}

TEST_CASE("daw/transport | Note end crosses through the loop wrap", "[daw]")
{
    // The off crossing is `len` beats after the on, mapped onto the loop:
    // a note longer than the loop's remainder ends on the next lap.
    static_assert(note_end_beat(3.5, 1.0, 0.0, 4.0, true) == 0.5);
    static_assert(note_end_beat(3.5, 1.0, 0.0, 4.0, false) == 4.5);
    static_assert(note_end_beat(1.0, 1.5, 0.0, 4.0, true) == 2.5);
    static_assert(note_end_beat(2.0, 1.0, 2.0, 6.0, true) == 3.0);
    // A note as long as the loop folds the off crossing back onto the on:
    // degenerate, both fire in one block, the gate treats it as over.
    static_assert(note_end_beat(3.5, 4.0, 0.0, 4.0, true) == 3.5);

    REQUIRE(note_end_beat(3.5, 1.0, 0.0, 4.0, true) == Catch::Approx(0.5));
    REQUIRE(note_end_beat(3.5, 1.0, 0.0, 4.0, false) == Catch::Approx(4.5));
    REQUIRE(note_end_beat(1.0, 1.5, 0.0, 4.0, true) == Catch::Approx(2.5));
    REQUIRE(note_end_beat(2.0, 1.0, 2.0, 6.0, true) == Catch::Approx(3.0));
    REQUIRE(note_end_beat(3.5, 4.0, 0.0, 4.0, true) == Catch::Approx(3.5));
}

TEST_CASE("daw/transport | Voice gate folds the twin crossings into one event", "[daw]")
{
    static_assert(voice_gate(0, 0) == 0);
    static_assert(voice_gate(1, 0) == 1);
    static_assert(voice_gate(0, 1) == -1);
    static_assert(voice_gate(2, 0) == 1);
    // A note entirely inside one block: the off wins — no sub-block envelope.
    static_assert(voice_gate(1, 1) == -1);

    REQUIRE(voice_gate(0, 0) == 0);
    REQUIRE(voice_gate(1, 0) == 1);
    REQUIRE(voice_gate(0, 1) == -1);
    REQUIRE(voice_gate(1, 1) == -1);
}

TEST_CASE("daw/transport | Voice opens on the note, closes on the off", "[daw]")
{
    // Silence sustains silence; the on opens and captures the id.
    static_assert(!voice_latch(Voice { false, -1 }, 0, 60).active);
    static_assert(voice_latch(Voice { false, -1 }, 1, 60).active);
    static_assert(voice_latch(Voice { false, -1 }, 1, 60).id == 60);

    // The off closes and clears the id; an off with nothing sounding is a
    // no-op, which is what makes the wrapped end beat harmless before the
    // first note-on.
    static_assert(!voice_latch(Voice { true, 60 }, -1, 62).active);
    static_assert(voice_latch(Voice { true, 60 }, -1, 62).id == -1);
    static_assert(!voice_latch(Voice { false, -1 }, -1, 60).active);

    // Retrigger while sounding captures the new id.
    static_assert(voice_latch(Voice { true, 60 }, 1, 62).id == 62);

    static_assert(voice_gain(Voice { false, -1 }, 0.8) == 0.0);
    static_assert(voice_gain(Voice { true, 60 }, 0.8) == 0.8);
}
