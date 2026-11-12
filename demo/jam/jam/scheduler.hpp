#pragma once

/// @file
/// Pure note scheduling over one audio-tick interval.
///
/// The transport advances by `block.n_frames` each tick; the scheduler reads
/// the current score and the tick's position pair and emits the note-on/off
/// crossings in `[prev, cur)` (a wrap splits the interval into the tail
/// `[prev, loop_end)` and the head `[loop_start, cur)`). The half-open
/// convention is deliberate: a note exactly at beat zero fires on the first
/// tick instead of waiting for a later block to cross it. A block spanning
/// more than one loop lap is treated as a single wrap — audio blocks are far
/// shorter than any useful loop.
///
/// Steps are 16th notes (`step_beats`), anchored to absolute musical beats —
/// the loop region is a transport window, not a pattern phase reset. Each
/// gated step lasts one step: its off crossing lands on the next boundary.
///
/// Everything is bounded: at most `event_capacity` events per tick, and the
/// overflow count is explicit. No allocation happens here; the tick path
/// writes into the returned value.

#include "jam/score.hpp"

#include "daw/transport.hpp"
#include "clockr/kernel/user_type.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace jam
{

inline constexpr double step_beats { 0.25 };
inline constexpr std::size_t event_capacity { 64 };

/// One scheduled crossing. `offset` is a sample offset inside the block;
/// `track` identifies the monophonic track it belongs to.
struct note_event
{
    std::int8_t note { 0 };
    std::uint8_t velocity { 0 };
    std::int16_t offset { 0 };
    std::uint8_t track { 0 };
    bool on { false };

    friend bool
    operator==(const note_event&, const note_event&) = default;
};

/// A tick's bounded event list. `silent` is a bitmask of tracks that must not
/// sound this tick (muted, invalid pattern, or zero length); the voice scan
/// releases those tracks immediately. `dropped` counts crossings that did not
/// fit (a lower bound when a whole trailing run of boundaries is skipped), so
/// overflow is observable rather than silent.
struct event_batch
{
    std::array<note_event, event_capacity> events {};
    std::uint8_t count { 0 };
    std::uint16_t dropped { 0 };
    std::uint8_t silent { 0 };

    friend bool
    operator==(const event_batch&, const event_batch&) = default;
};

namespace detail
{

inline void
add_dropped(event_batch& out, std::uint64_t count) noexcept
{
    const auto max { static_cast<std::uint32_t>(
        std::numeric_limits<std::uint16_t>::max()) };
    const auto room { max - static_cast<std::uint32_t>(out.dropped) };
    const auto add { static_cast<std::uint32_t>(
        std::min<std::uint64_t>(count, room)) };

    out.dropped = static_cast<std::uint16_t>(out.dropped + add);
}

inline void
push(event_batch& out, const note_event& event) noexcept
{
    if (out.count >= event_capacity)
    {
        add_dropped(out, 1);
        return;
    }

    out.events[out.count] = event;
    out.count = static_cast<std::uint8_t>(out.count + 1);
}

[[nodiscard]] inline std::size_t
pattern_length(const pattern& p) noexcept
{
    return std::min<std::size_t>(p.length, steps_per_pattern);
}

[[nodiscard]] inline std::int8_t
transposed_note(const step& s, std::int8_t transpose) noexcept
{
    const auto value { static_cast<int>(s.note) + static_cast<int>(transpose) };

    if (value < 0)
        return 0;
    if (value > 127)
        return 127;
    return static_cast<std::int8_t>(value);
}

[[nodiscard]] inline std::uint8_t
clamped_velocity(const step& s) noexcept
{
    return std::min<std::uint8_t>(s.velocity, 127);
}

[[nodiscard]] inline std::int16_t
block_offset(double beat,
             double prev,
             bool wrapped_head,
             double loop_end,
             double loop_start,
             const daw::BlockDesc& block,
             double bpm) noexcept
{
    const auto virtual_beat
    {
        wrapped_head ? loop_end + (beat - loop_start) : beat
    };
    const auto sample { daw::beat_to_sample(
        virtual_beat, prev, block.start_sample, bpm, block.sample_rate) };
    auto offset { sample - block.start_sample };

    if (offset < 0)
        offset = 0;

    const auto limit
    {
        std::min<std::int64_t>(
            block.n_frames > 0 ? block.n_frames - 1 : 0,
            std::numeric_limits<std::int16_t>::max())
    };

    if (offset > limit)
        offset = limit;

    return static_cast<std::int16_t>(offset);
}

inline void
emit_boundary(event_batch& out,
              const score& s,
              std::int64_t grid_step,
              double beat,
              double prev,
              bool wrapped_head,
              const daw::LoopRegion& loop,
              const daw::BlockDesc& block,
              double bpm) noexcept
{
    const auto offset { block_offset(
        beat, prev, wrapped_head, loop.end, loop.start, block, bpm) };

    for (std::size_t track_index { 0 }; track_index < track_count; ++track_index)
    {
        const auto bit { static_cast<std::uint8_t>(1u << track_index) };
        if ((out.silent & bit) != 0)
            continue;

        const auto& track { s.tracks[track_index] };
        if (track.pattern >= pattern_count)
            continue;

        const auto& p { s.patterns[track.pattern] };
        const auto length { pattern_length(p) };
        if (length == 0)
            continue;

        const auto len { static_cast<std::int64_t>(length) };
        auto local { grid_step % len };
        if (local < 0)
            local += len;

        const auto current { static_cast<std::size_t>(local) };
        const auto previous { static_cast<std::size_t>((local + len - 1) % len) };

        const auto& before { p.steps[previous] };
        if (before.gate && before.velocity > 0)
            push(out,
                 note_event {
                     .note = transposed_note(before, track.transpose),
                     .velocity = 0,
                     .offset = offset,
                     .track = static_cast<std::uint8_t>(track_index),
                     .on = false,
                 });

        const auto& now { p.steps[current] };
        if (now.gate && now.velocity > 0)
            push(out,
                 note_event {
                     .note = transposed_note(now, track.transpose),
                     .velocity = clamped_velocity(now),
                     .offset = offset,
                     .track = static_cast<std::uint8_t>(track_index),
                     .on = true,
                 });
    }
}

inline void
emit_segment(event_batch& out,
             const score& s,
             double begin,
             double end,
             double prev,
             bool wrapped_head,
             const daw::LoopRegion& loop,
             const daw::BlockDesc& block,
             double bpm) noexcept
{
    if (!(end > begin))
        return;

    constexpr double beat_limit { 1.0e9 };
    if (begin < -beat_limit || end > beat_limit)
        return;

    const auto first
    {
        static_cast<std::int64_t>(std::ceil(begin / step_beats))
    };
    const auto last
    {
        static_cast<std::int64_t>(std::ceil(end / step_beats)) - 1
    };

    if (last < first)
        return;

    for (auto grid_step { first }; grid_step <= last; ++grid_step)
    {
        if (out.count >= event_capacity)
        {
            add_dropped(
                out,
                static_cast<std::uint64_t>(last - grid_step + 1));
            break;
        }

        emit_boundary(
            out,
            s,
            grid_step,
            static_cast<double>(grid_step) * step_beats,
            prev,
            wrapped_head,
            loop,
            block,
            bpm);
    }
}

} // namespace detail

/// Schedule one audio tick. Pure over its inputs; the result is bounded by
/// `event_capacity` and every overflow is counted in `dropped`.
[[nodiscard]] inline event_batch
schedule(const score& s,
         const daw::PosPair& pos,
         const daw::LoopRegion& loop,
         const daw::BlockDesc& block,
         double bpm) noexcept
{
    event_batch out {};

    for (std::size_t track_index { 0 }; track_index < track_count; ++track_index)
    {
        const auto& track { s.tracks[track_index] };
        const bool silent { track.mute
                            || track.pattern >= pattern_count
                            || detail::pattern_length(s.patterns[track.pattern]) == 0 };

        if (silent)
            out.silent = static_cast<std::uint8_t>(
                out.silent | static_cast<std::uint8_t>(1u << track_index));
    }

    if (block.n_frames <= 0 || block.sample_rate <= 0.0 || !(bpm > 0.0))
        return out;

    if (!std::isfinite(pos.prev) || !std::isfinite(pos.cur)
        || !std::isfinite(loop.start) || !std::isfinite(loop.end))
        return out;

    const bool looped { loop.enabled && loop.end > loop.start };

    if (pos.cur < pos.prev)
    {
        if (!looped)
            return out;

        detail::emit_segment(
            out, s, pos.prev, loop.end, pos.prev, false, loop, block, bpm);
        detail::emit_segment(
            out, s, loop.start, pos.cur, pos.prev, true, loop, block, bpm);
    }
    else if (pos.cur > pos.prev)
        detail::emit_segment(
            out, s, pos.prev, pos.cur, pos.prev, false, loop, block, bpm);

    return out;
}

} // namespace jam

// --- debugger schemas --------------------------------------------------------

namespace clockr::kernel
{

template<>
struct describe<jam::note_event>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("note", &jam::note_event::note),
            field("velocity", &jam::note_event::velocity),
            field("offset", &jam::note_event::offset),
            field("track", &jam::note_event::track),
            field("on", &jam::note_event::on),
        };
        static const user_type t { make_user_type<jam::note_event>("jam.note_event", fields) };

        return t;
    }
};

template<>
struct describe<jam::event_batch>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("events", &jam::event_batch::events),
            field("count", &jam::event_batch::count),
            field("dropped", &jam::event_batch::dropped),
            field("silent", &jam::event_batch::silent),
        };
        static const user_type t { make_user_type<jam::event_batch>("jam.event_batch", fields) };

        return t;
    }
};

} // namespace clockr::kernel
