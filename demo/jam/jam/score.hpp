#pragma once

/// @file
/// The jam's musical model: bounded, trivially-copyable data.
///
/// A score is 16 patterns (pattern.hpp) of 32 steps plus 8 tracks. Everything
/// is plain aggregate data; the kernel carries a `score` through a
/// ref-counted pool cell automatically (it is far above the 16-byte inline
/// budget). Edits are values (`score_patch`, see patch.hpp), never topology
/// changes and never mutable state captured by a scheduler.

#include "jam/pattern.hpp"

#include "clockr/kernel/user_type.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace jam
{

inline constexpr std::size_t pattern_count { 16 };
inline constexpr std::size_t track_count { 8 };

/// A track plays one pattern, transposed and scaled. Tracks default to muted
/// so a fresh score is silent even if a pattern is authored in slot 0.
struct track
{
    std::uint8_t pattern { 0 };
    std::int8_t transpose { 0 };
    std::uint8_t gain { 200 }; // 0..255, scaled by 1/255 in the mix
    bool mute { true };

    friend bool
    operator==(const track&, const track&) = default;
};

/// The editable score. `bpm` is authoring metadata; the live `bpm` port is
/// what the network's transport uses (see network.hpp).
struct score
{
    std::array<pattern, pattern_count> patterns {};
    std::array<track, track_count> tracks {};
    std::uint8_t bpm { 120 };

    friend bool
    operator==(const score&, const score&) = default;
};

} // namespace jam

// --- debugger schemas --------------------------------------------------------

namespace clockr::kernel
{

template<>
struct describe<jam::track>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("pattern", &jam::track::pattern),
            field("transpose", &jam::track::transpose),
            field("gain", &jam::track::gain),
            field("mute", &jam::track::mute),
        };
        static const user_type t { make_user_type<jam::track>("jam.track", fields) };

        return t;
    }
};

template<>
struct describe<jam::score>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("patterns", &jam::score::patterns),
            field("tracks", &jam::score::tracks),
            field("bpm", &jam::score::bpm),
        };
        static const user_type t { make_user_type<jam::score>("jam.score", fields) };

        return t;
    }
};

} // namespace clockr::kernel
