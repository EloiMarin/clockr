#pragma once

/// @file
/// Pure, total score edits. A `score_patch` is a bounded list of operations;
/// `apply` copies the score, applies the operations in order, and returns the
/// new score. Invalid indices and out-of-range parameters are clamped or
/// ignored — never undefined behaviour.
///
/// Operation fields (all indices are 0-based; pattern 0..15, step 0..31,
/// track 0..7):
///
/// | op          | a          | b                 | c           | d          |
/// | ----------- | ---------- | ----------------- | ----------- | ---------- |
/// | set_step    | pattern    | step              | note 0..127 | velocity   |
/// | clear_step  | pattern    | step              | -           | -          |
/// | transpose   | pattern    | -                 | -           | semitones  |
/// | rotate      | pattern    | -                 | -           | shift      |
/// | mask        | pattern    | 8-bit mask        | -           | -          |
/// | density     | pattern    | keep count        | period      | -          |
/// | stack       | destination| source pattern    | -           | -          |
/// | sequence    | destination| source pattern    | -           | -          |
///
/// `set_step` also extends the pattern length to cover the written step when
/// velocity is non-zero; `clear_step` silences a step but leaves the length
/// alone. `transpose` shifts every gated step of a pattern and clamps to MIDI
/// 0..127. `rotate` rotates the active length by `d` positions (positive
/// toward higher indices). `mask` keeps gated steps whose bit is set in the
/// repeating 8-bit `b` mask. `density` keeps the first `b` of every `c`
/// active positions. `stack` ORs the source into the destination (destination
/// wins on collisions) and extends to the longer length. `sequence` appends
/// the source's active steps after the destination's, truncating at 32.

#include "jam/score.hpp"

#include "clockr/kernel/user_type.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace jam
{

inline constexpr std::size_t patch_capacity { 32 };

enum class patch_op : std::uint8_t
{
    set_step,
    clear_step,
    transpose,
    rotate,
    mask,
    density,
    stack,
    sequence,
};

/// One patch operation; the meaning of `a`..`d` depends on `op` (see above).
struct patch_step
{
    patch_op op { patch_op::clear_step };
    std::uint8_t a { 0 };
    std::uint8_t b { 0 };
    std::uint8_t c { 0 };
    std::int16_t d { 0 };

    friend bool
    operator==(const patch_step&, const patch_step&) = default;
};

/// A bounded operation list. `count` above `patch_capacity` is clamped.
struct score_patch
{
    std::array<patch_step, patch_capacity> ops {};
    std::uint8_t count { 0 };

    friend bool
    operator==(const score_patch&, const score_patch&) = default;
};

namespace detail
{

[[nodiscard]] inline std::uint8_t
clamp_velocity(int value) noexcept
{
    if (value < 0)
        return 0;
    if (value > 127)
        return 127;
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] inline std::int8_t
clamp_note(int value) noexcept
{
    if (value < 0)
        return 0;
    if (value > 127)
        return 127;
    return static_cast<std::int8_t>(value);
}

[[nodiscard]] inline std::size_t
active_length(const pattern& p) noexcept
{
    return std::min<std::size_t>(p.length, steps_per_pattern);
}

[[nodiscard]] inline pattern*
pattern_at(score& s, std::uint8_t index) noexcept
{
    if (index >= pattern_count)
        return nullptr;
    return &s.patterns[index];
}

inline void
apply_set_step(score& s, const patch_step& op) noexcept
{
    auto* p { pattern_at(s, op.a) };
    if (p == nullptr || op.b >= steps_per_pattern)
        return;

    const auto velocity { clamp_velocity(op.d) };
    p->steps[op.b] = step { clamp_note(op.c), velocity, velocity > 0 };

    if (velocity > 0)
    {
        const auto needed { static_cast<std::uint8_t>(op.b + 1) };
        p->length = std::max(p->length, needed);
    }
}

inline void
apply_clear_step(score& s, const patch_step& op) noexcept
{
    auto* p { pattern_at(s, op.a) };
    if (p == nullptr || op.b >= steps_per_pattern)
        return;

    p->steps[op.b] = step { 60, 0, false };
}

inline void
apply_transpose(score& s, const patch_step& op) noexcept
{
    auto* p { pattern_at(s, op.a) };
    if (p == nullptr)
        return;

    const auto shift { static_cast<int>(op.d) };
    const auto length { active_length(*p) };

    for (std::size_t i { 0 }; i < length; ++i)
    {
        auto& st { p->steps[i] };
        if (!st.gate || st.velocity == 0)
            continue;

        st.note = clamp_note(static_cast<int>(st.note) + shift);
    }
}

inline void
apply_rotate(score& s, const patch_step& op) noexcept
{
    auto* p { pattern_at(s, op.a) };
    if (p == nullptr)
        return;

    const auto length { active_length(*p) };
    if (length <= 1)
        return;

    const auto len { static_cast<std::int64_t>(length) };
    auto shift { static_cast<std::int64_t>(op.d) % len };
    if (shift < 0)
        shift += len;
    if (shift == 0)
        return;

    std::array<step, steps_per_pattern> rotated {};
    for (std::size_t i { 0 }; i < length; ++i)
    {
        const auto target { static_cast<std::size_t>(
            (static_cast<std::int64_t>(i) + shift) % len) };
        rotated[target] = p->steps[i];
    }

    for (std::size_t i { 0 }; i < length; ++i)
        p->steps[i] = rotated[i];
}

inline void
apply_mask(score& s, const patch_step& op) noexcept
{
    auto* p { pattern_at(s, op.a) };
    if (p == nullptr)
        return;

    const auto length { active_length(*p) };

    for (std::size_t i { 0 }; i < length; ++i)
    {
        const auto bit { static_cast<std::uint8_t>(i % 8) };
        if (((static_cast<std::uint8_t>(op.b >> bit)) & 1u) == 0)
            p->steps[i] = step { 60, 0, false };
    }
}

inline void
apply_density(score& s, const patch_step& op) noexcept
{
    auto* p { pattern_at(s, op.a) };
    if (p == nullptr || op.c == 0)
        return;

    const auto length { active_length(*p) };
    const auto period { static_cast<std::size_t>(op.c) };
    const auto keep { std::min<std::size_t>(op.b, period) };

    for (std::size_t i { 0 }; i < length; ++i)
        if (i % period >= keep)
            p->steps[i] = step { 60, 0, false };
}

inline void
apply_stack(score& s, const patch_step& op) noexcept
{
    auto* dst { pattern_at(s, op.a) };
    if (dst == nullptr || op.b >= pattern_count)
        return;

    const auto* src { &s.patterns[op.b] };
    if (dst == src)
        return;

    const auto dst_length { active_length(*dst) };
    const auto src_length { active_length(*src) };
    const auto length { std::min<std::size_t>(
        std::max(dst_length, src_length), steps_per_pattern) };

    for (std::size_t i { 0 }; i < length; ++i)
    {
        const auto& from { src->steps[i] };
        auto& into { dst->steps[i] };

        if ((!into.gate || into.velocity == 0)
            && from.gate && from.velocity > 0)
            into = from;
    }

    dst->length = static_cast<std::uint8_t>(length);
}

inline void
apply_sequence(score& s, const patch_step& op) noexcept
{
    auto* dst { pattern_at(s, op.a) };
    if (dst == nullptr || op.b >= pattern_count)
        return;

    const auto* src { &s.patterns[op.b] };
    if (dst == src)
        return;

    const auto dst_length { active_length(*dst) };
    const auto src_length { active_length(*src) };
    const auto room { steps_per_pattern - dst_length };
    const auto copied { std::min(src_length, room) };

    for (std::size_t i { 0 }; i < copied; ++i)
        dst->steps[dst_length + i] = src->steps[i];

    dst->length = static_cast<std::uint8_t>(dst_length + copied);
}

inline void
apply_one(score& s, const patch_step& op) noexcept
{
    switch (op.op)
    {
        case patch_op::set_step:
            apply_set_step(s, op);
            break;
        case patch_op::clear_step:
            apply_clear_step(s, op);
            break;
        case patch_op::transpose:
            apply_transpose(s, op);
            break;
        case patch_op::rotate:
            apply_rotate(s, op);
            break;
        case patch_op::mask:
            apply_mask(s, op);
            break;
        case patch_op::density:
            apply_density(s, op);
            break;
        case patch_op::stack:
            apply_stack(s, op);
            break;
        case patch_op::sequence:
            apply_sequence(s, op);
            break;
        default:
            break;
    }
}

} // namespace detail

/// Apply a patch to a score. Pure and total: the input is not modified, the
/// result is a fresh score, and out-of-range operations are no-ops or clamped.
[[nodiscard]] inline score
apply(const score& in, const score_patch& patch) noexcept
{
    score out { in };
    const auto count { std::min<std::size_t>(patch.count, patch_capacity) };

    for (std::size_t i { 0 }; i < count; ++i)
        detail::apply_one(out, patch.ops[i]);

    return out;
}

} // namespace jam

// --- debugger schemas --------------------------------------------------------

namespace clockr::kernel
{

template<>
struct describe<jam::patch_step>
{
    static const user_type& type()
    {
        static const std::array operations
        {
            enum_entry { 0, "set_step" },
            enum_entry { 1, "clear_step" },
            enum_entry { 2, "transpose" },
            enum_entry { 3, "rotate" },
            enum_entry { 4, "mask" },
            enum_entry { 5, "density" },
            enum_entry { 6, "stack" },
            enum_entry { 7, "sequence" },
        };
        static const std::array fields
        {
            enum_field("op", &jam::patch_step::op, operations),
            field("a", &jam::patch_step::a),
            field("b", &jam::patch_step::b),
            field("c", &jam::patch_step::c),
            field("d", &jam::patch_step::d),
        };
        static const user_type t { make_user_type<jam::patch_step>("jam.patch_step", fields) };

        return t;
    }
};

template<>
struct describe<jam::score_patch>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("ops", &jam::score_patch::ops),
            field("count", &jam::score_patch::count),
        };
        static const user_type t { make_user_type<jam::score_patch>("jam.score_patch", fields) };

        return t;
    }
};

} // namespace clockr::kernel
