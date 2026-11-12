#pragma once

/// @file
/// Fixed-size polyphony over a tick's event batch.
///
/// The jam has eight tracks and eight voices. A note-on first releases any
/// voice on the same track (tracks are monophonic), then takes an inactive
/// voice; when all voices are busy it steals the oldest (smallest serial).
/// Note-offs release every voice on their track. Tracks marked silent in the
/// batch are released before any event is applied, so a mute or a cleared
/// pattern cannot leave a voice stuck.

#include "jam/scheduler.hpp"
#include "jam/score.hpp"

#include "clockr/kernel/user_type.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace jam
{

inline constexpr std::size_t voice_count { 8 };

/// One voice's latched state. `serial` orders allocations for stealing.
struct voice
{
    bool active { false };
    std::int8_t note { 0 };
    std::uint8_t velocity { 0 };
    std::uint8_t track { 0 };
    std::uint32_t serial { 0 };

    friend bool
    operator==(const voice&, const voice&) = default;
};

/// The voice pool and its monotonic allocation stamp.
struct voice_state
{
    std::array<voice, voice_count> slots {};
    std::uint32_t next_serial { 1 };

    friend bool
    operator==(const voice_state&, const voice_state&) = default;
};

namespace detail
{

inline void
release(voice& v) noexcept
{
    v.active = false;
    v.note = 0;
    v.velocity = 0;
    v.track = 0;
    v.serial = 0;
}

[[nodiscard]] inline bool
track_is_silent(const event_batch& batch, std::uint8_t track) noexcept
{
    const auto bit { static_cast<std::uint8_t>(1u << track) };
    return (batch.silent & bit) != 0;
}

} // namespace detail

/// Advance the voice pool by one event batch. Deterministic and bounded.
[[nodiscard]] inline voice_state
voice_tick(const voice_state& prev, const event_batch& batch) noexcept
{
    voice_state next { prev };

    for (auto& v : next.slots)
        if (v.active && detail::track_is_silent(batch, v.track))
            detail::release(v);

    const auto events { std::min<std::size_t>(batch.count, event_capacity) };

    for (std::size_t i { 0 }; i < events; ++i)
    {
        const auto& event { batch.events[i] };

        if (!event.on)
        {
            for (auto& v : next.slots)
                if (v.active && v.track == event.track)
                    detail::release(v);

            continue;
        }

        for (auto& v : next.slots)
            if (v.active && v.track == event.track)
                detail::release(v);

        auto slot { voice_count };

        for (std::size_t candidate { 0 }; candidate < voice_count; ++candidate)
            if (!next.slots[candidate].active)
            {
                slot = candidate;
                break;
            }

        if (slot == voice_count)
        {
            slot = 0;

            for (std::size_t candidate { 1 }; candidate < voice_count; ++candidate)
                if (next.slots[candidate].serial < next.slots[slot].serial)
                    slot = candidate;
        }

        auto& v { next.slots[slot] };
        v.active = true;
        v.note = event.note;
        v.velocity = event.velocity;
        v.track = event.track;
        v.serial = next.next_serial;

        ++next.next_serial;
        if (next.next_serial == 0)
            next.next_serial = 1;
    }

    return next;
}

/// Sum the active voices' gain into one bounded mix level. Track gain and
/// note velocity are both normalised; the sum is clamped to unity before the
/// master level is applied, so the output stays a safe `double` gain.
[[nodiscard]] inline double
voice_mix(const voice_state& voices,
          const score& s,
          double master) noexcept
{
    double sum { 0.0 };

    for (const auto& v : voices.slots)
    {
        if (!v.active || v.track >= track_count)
            continue;

        const auto& track { s.tracks[v.track] };
        if (track.mute)
            continue;

        sum += (static_cast<double>(track.gain) / 255.0)
            * (static_cast<double>(v.velocity) / 127.0);
    }

    if (sum > 1.0)
        sum = 1.0;
    if (sum < 0.0)
        sum = 0.0;

    if (master < 0.0)
        master = 0.0;
    if (master > 1.0)
        master = 1.0;

    return sum * master;
}

} // namespace jam

// --- debugger schemas --------------------------------------------------------

namespace clockr::kernel
{

template<>
struct describe<jam::voice>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("active", &jam::voice::active),
            field("note", &jam::voice::note),
            field("velocity", &jam::voice::velocity),
            field("track", &jam::voice::track),
            field("serial", &jam::voice::serial),
        };
        static const user_type t { make_user_type<jam::voice>("jam.voice", fields) };

        return t;
    }
};

template<>
struct describe<jam::voice_state>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("slots", &jam::voice_state::slots),
            field("next_serial", &jam::voice_state::next_serial),
        };
        static const user_type t { make_user_type<jam::voice_state>("jam.voice_state", fields) };

        return t;
    }
};

} // namespace clockr::kernel
