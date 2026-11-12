#pragma once

/// @file
/// Steps and patterns: the smallest editable musical units.
///
/// A pattern is 32 steps, of which the first `length` are active. Both types
/// are plain, trivially-copyable data; a score (score.hpp) is a fixed array
/// of them, so the whole musical model travels as an ordinary user value.

#include "clockr/kernel/user_type.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace jam
{

inline constexpr std::size_t steps_per_pattern { 32 };

/// One step of a pattern. `gate` (with non-zero velocity) makes the step
/// sound; `note` is a MIDI note number and `velocity` is 0..127.
struct step
{
    std::int8_t note { 60 };
    std::uint8_t velocity { 0 };
    bool gate { false };

    friend bool
    operator==(const step&, const step&) = default;
};

/// A pattern: up to 32 steps, of which the first `length` are active. A zero
/// length is silent. Steps past `length` are inert.
struct pattern
{
    std::array<step, steps_per_pattern> steps {};
    std::uint8_t length { 0 };

    friend bool
    operator==(const pattern&, const pattern&) = default;
};

} // namespace jam

// --- debugger schemas --------------------------------------------------------

namespace clockr::kernel
{

template<>
struct describe<jam::step>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("note", &jam::step::note),
            field("velocity", &jam::step::velocity),
            field("gate", &jam::step::gate),
        };
        static const user_type t { make_user_type<jam::step>("jam.step", fields) };

        return t;
    }
};

template<>
struct describe<jam::pattern>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("steps", &jam::pattern::steps),
            field("length", &jam::pattern::length),
        };
        static const user_type t { make_user_type<jam::pattern>("jam.pattern", fields) };

        return t;
    }
};

} // namespace clockr::kernel
