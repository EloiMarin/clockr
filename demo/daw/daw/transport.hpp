#pragma once

// daw transport math — pure, total, engine-free (README §3–§5).
//
// Everything here is ordinary C++: no recipe, no graph, no engine. The FRP
// wiring in daw/network.hpp adds no logic, it only places these functions on
// clocks. Unit-test this file alone (§6.4 case table).
//
// The wire types are cohesive structs, carried by kernel::value: small ones
// (PosPair, Voice) ride inline with value semantics, the rest
// (LoopRegion, BlockDesc, TransportIn) ride ref-counted pool cells. That is
// what dissolved the old scalar discipline — one `scan` over TransportIn
// holds the whole tick bundle, so the transport is a single equation again
// instead of a rebase decomposition. Purely authoring-level bundles (leaves
// that never cross a clock or persist in state) stay phantom pairs: structs
// on wires are the paying option, not the spelling option (see README §4).

#include "clockr/kernel/user_type.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace daw
{

// --- wire structs ------------------------------------------------------------
//
// Trivially-copyable aggregates: inline when they fit the 16-byte budget
// (PosPair, Voice), pooled cells otherwise. `describe` schemas below let the
// debugger render every wire struct inline with its fields.

struct BlockDesc // audio input, one per block tick
{
    std::int64_t start_sample { 0 }; // first sample of this block (transport-absolute)
    std::int64_t n_frames { 0 };     // block size
    double sample_rate { 48000.0 };
};

struct LoopRegion
{
    double start { 0.0 };
    double end { 4.0 };
    bool enabled { false };
};

struct TransportIn // one audio tick's intent bundle
{
    BlockDesc block {};
    LoopRegion loop {};
    bool playing { false };
    std::optional<double> seek {}; // engaged = seek wins; disengaged = integrate
    double bpm { 120.0 };
};

struct PosPair // the joint scan state: previous and current position
{
    double prev { 0.0 };
    double cur { 0.0 };
};

struct Voice // monophonic voice state: latched id, silence sustains
{
    bool active { false };
    std::int64_t id { -1 };
};

// Beats in one block at constant tempo (K = 1 advance; a K-point curve stays
// a bounded walk over closure-captured points in the lift body — same shape,
// no new wires).
[[nodiscard]] inline constexpr double
beats_per_block(std::int64_t n_frames, double sample_rate, double bpm) noexcept
{
    return static_cast<double>(n_frames) * bpm / (60.0 * sample_rate);
}

// Loop remainder, not clamp. Degenerate (end <= start) or disabled loops pass
// through; multi-wrap terminates on the loop length.
[[nodiscard]] inline constexpr double
wrap_pos(double p, double start, double end, bool on) noexcept
{
    if (!on || end <= start)
        return p;

    if (p < end)
        return p;

    const double len { end - start };
    double off { p - start };

    while (off >= len)
        off -= len;

    return start + off;
}

// transport_step: THE transport equation, wired directly into one scan —
//   s(t) = f(s(t-1), in(t)), s = {prev, cur}, s(-1) = {0, 0},
//   f({pp, pc}, in) = {pc, transport_step(pc, in)} —
//   f(prev,in) = seek? -> wrap(*seek) | !playing? -> prev | else wrap(prev+advance).
// This is the executable spec of the wired transport: the §6.1 engine test
// proves the wire agrees with it tick for tick.
[[nodiscard]] inline constexpr double
transport_step(double prev, const TransportIn& in)
{
    if (in.seek)
        // Seek wins, even paused — and lands through the same wrap policy as
        // an advance (loop on: wrap once; off: pinned), remainder discarded.
        return wrap_pos(*in.seek, in.loop.start, in.loop.end, in.loop.enabled);

    if (!in.playing)
        return prev;

    return wrap_pos(
        prev + beats_per_block(in.block.n_frames, in.block.sample_rate, in.bpm),
        in.loop.start,
        in.loop.end,
        in.loop.enabled);
}

// Beat at this block's start: the seek when seeking, else the previous
// (wrapped) position. The schedulers fire over (block_start, cur].
[[nodiscard]] inline constexpr double
block_start_beat(double prev, const std::optional<double>& seek) noexcept
{
    return seek.value_or(prev);
}

// Pure affine map for sample-accurate offsets; exact under constant tempo
// across the block (the P1 tempo model), which is all the offset needs.
[[nodiscard]] inline constexpr std::int64_t
beat_to_sample(double beat,
               double block_start_beat_v,
               std::int64_t block_start_sample,
               double bpm,
               double rate) noexcept
{
    const double seconds { (beat - block_start_beat_v) * 60.0 / bpm };
    return block_start_sample + static_cast<std::int64_t>(seconds * rate);
}

// Monophonic crossing count over (bstart, cur], splitting across a loop wrap
// into (bstart, end] + (start, cur] (§6.4 case 6). The half-open interval
// means a note exactly on a block boundary fires once, on the later block.
[[nodiscard]] inline constexpr std::int64_t
schedule_count(double bstart,
               double cur,
               double loop_start,
               double loop_end,
               bool loop_on,
               double note_beat) noexcept
{
    const auto fires = [note_beat](double a, double b) noexcept
    {
        return note_beat > a && note_beat <= b;
    };

    if (loop_on && loop_end > loop_start && cur < bstart)
        return (fires(bstart, loop_end) || fires(loop_start, cur)) ? 1 : 0;

    return fires(bstart, cur) ? 1 : 0;
}

// First-trigger sample offset within the block, or -1 when nothing fires.
// Same firing equation as schedule_count; clamped defensively to the block.
[[nodiscard]] inline constexpr std::int64_t
schedule_offset(double bstart,
                double cur,
                double loop_start,
                double loop_end,
                bool loop_on,
                std::int64_t block_start_sample,
                std::int64_t block_n,
                double rate,
                double bpm,
                double note_beat) noexcept
{
    if (block_n <= 0)
        return -1;

    const auto fires = [note_beat](double a, double b) noexcept
    {
        return note_beat > a && note_beat <= b;
    };

    bool found { false };
    double fired { 0.0 };

    const auto consider = [&](double a, double b) noexcept
    {
        if (!found && fires(a, b))
        {
            fired = note_beat;
            found = true;
        }
    };

    if (loop_on && loop_end > loop_start && cur < bstart)
    {
        consider(bstart, loop_end);
        consider(loop_start, cur);
    }
    else
        consider(bstart, cur);

    if (!found)
        return -1;

    std::int64_t off { beat_to_sample(fired, bstart, block_start_sample, bpm, rate) - block_start_sample };

    if (off < 0)
        off = 0;

    if (off >= block_n)
        off = block_n - 1;

    return off;
}

// The note-off crossing beat: the transport reaches it exactly `len` beats
// after the note-on, mapped onto the loop — a note longer than the loop's
// remainder ends on the next lap, which an absolute end beat could not
// express (the transport never reaches beats past the loop end). With the
// loop off it is the plain absolute end. Degenerate lengths (0, or exactly
// the loop length) collapse the off crossing onto the on: the note blips
// inside a single block and the gate treats it as over (voice_gate below).
[[nodiscard]] inline constexpr double
note_end_beat(double start_beat, double len, double loop_start, double loop_end, bool loop_on) noexcept
{
    return wrap_pos(start_beat + len, loop_start, loop_end, loop_on);
}

// The block's gate event for the voice scan: +1 a note-on crossing fired,
// -1 a note-off crossing fired, 0 neither. A block containing both crossings
// held a note entirely inside one block — the off wins: P1 gates at block
// granularity and has no sub-block envelope (the offset output still pins
// where the blip landed). An off crossing with nothing sounding is a no-op
// by the latch below.
[[nodiscard]] inline constexpr std::int64_t
voice_gate(std::int64_t on_count, std::int64_t off_count) noexcept
{
    if (on_count > 0 && off_count > 0)
        return -1;
    if (on_count > 0)
        return 1;
    if (off_count > 0)
        return -1;
    return 0;
}

// Single-voice latch over a struct state: the gate opens the note (+1
// captures the id), closes it (-1), silence sustains (0). The N-voice pool
// is this function mapped over an array state — array wires are future
// work, so P1 voices monophonically and the steal policy waits with it.
[[nodiscard]] inline constexpr Voice
voice_latch(Voice prev, std::int64_t gate, std::int64_t note_id) noexcept
{
    if (gate > 0)
        return Voice { true, note_id };
    if (gate < 0)
        return Voice { false, -1 };
    return prev;
}

[[nodiscard]] inline constexpr double
voice_gain(const Voice& voice, double master) noexcept
{
    return voice.active ? master : 0.0;
}

} // namespace daw

// --- debugger schemas --------------------------------------------------------
//
// `describe` specializations so the debugger renders DAW values inline as
// `type{field=value, …}` instead of bare `struct` placeholders — including
// the optional `seek`, which renders its beats or absent.

namespace clockr::kernel
{

template<>
struct describe<daw::BlockDesc>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("start_sample", &daw::BlockDesc::start_sample),
            field("n_frames", &daw::BlockDesc::n_frames),
            field("sample_rate", &daw::BlockDesc::sample_rate),
        };
        static const user_type t { make_user_type<daw::BlockDesc>("daw.block_desc", fields) };

        return t;
    }
};

template<>
struct describe<daw::LoopRegion>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("start", &daw::LoopRegion::start),
            field("end", &daw::LoopRegion::end),
            field("enabled", &daw::LoopRegion::enabled),
        };
        static const user_type t { make_user_type<daw::LoopRegion>("daw.loop_region", fields) };

        return t;
    }
};

template<>
struct describe<daw::PosPair>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("prev", &daw::PosPair::prev),
            field("cur", &daw::PosPair::cur),
        };
        static const user_type t { make_user_type<daw::PosPair>("daw.pos_pair", fields) };

        return t;
    }
};

template<>
struct describe<daw::Voice>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("active", &daw::Voice::active),
            field("id", &daw::Voice::id),
        };
        static const user_type t { make_user_type<daw::Voice>("daw.voice", fields) };

        return t;
    }
};

template<>
struct describe<daw::TransportIn>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("block", &daw::TransportIn::block),
            field("loop", &daw::TransportIn::loop),
            field("playing", &daw::TransportIn::playing),
            field("seek", &daw::TransportIn::seek),
            field("bpm", &daw::TransportIn::bpm),
        };
        static const user_type t { make_user_type<daw::TransportIn>("daw.transport_in", fields) };

        return t;
    }
};

} // namespace clockr::kernel
