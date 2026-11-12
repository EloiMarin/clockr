#pragma once

/// @file
/// Deterministic musical metrics over a score or a rendered event batch.
///
/// These are analysis aids, not quality judgements. They are pure, bounded,
/// allocation-free functions with no I/O and no engine dependency, so the same
/// inputs always produce the same numbers on every platform.
///
/// Score metrics consider the audible tracks only (a track that is muted or
/// names an invalid pattern contributes nothing):
///
/// | field                 | definition |
/// | --------------------- | ---------- |
/// | `event_density`       | gated steps / active steps, summed across audible
/// tracks | | `simultaneity`        | largest number of gated steps in one grid
/// column | | `register_spread`     | max effective MIDI note - min effective
/// note | | `duplicate_notes`     | sounding notes beyond the first on the same
/// note/column | | `unresolved_sustains` | 0: a score step is a one-step gate,
/// never a sustain | | `clipping_risk`       | worst column's sum of `gain/255
/// * velocity/127` | | `voice_pressure`      | 0: a score has no rendered
/// voices |
///
/// Event metrics use the tick's batch and the voice state it produced:
///
/// | field                 | definition |
/// | --------------------- | ---------- |
/// | `event_density`       | note-on events / `event_capacity` |
/// | `simultaneity`        | largest number of onsets sharing one offset |
/// | `register_spread`     | max note - min note among onsets |
/// | `duplicate_notes`     | onsets beyond the first on the same note/offset |
/// | `unresolved_sustains` | onsets with no later note-off on the same track |
/// | `clipping_risk`       | worst offset's sum of `velocity/127` |
/// | `voice_pressure`      | active voices in the supplied voice state |

#include "jam/scheduler.hpp"
#include "jam/score.hpp"
#include "jam/voice.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace jam
{

struct metrics
{
    double event_density { 0.0 };
    std::int64_t simultaneity { 0 };
    std::int64_t register_spread { 0 };
    std::int64_t duplicate_notes { 0 };
    std::int64_t unresolved_sustains { 0 };
    double clipping_risk { 0.0 };
    std::int64_t voice_pressure { 0 };
};

namespace detail
{

[[nodiscard]] constexpr bool
sounding(const step& s) noexcept
{
    return s.gate && s.velocity > 0;
}

[[nodiscard]] constexpr std::int32_t
effective_note(std::int8_t note, std::int8_t transpose) noexcept
{
    const auto value { static_cast<std::int32_t>(note) +
                       static_cast<std::int32_t>(transpose) };

    if (value < 0)
        return 0;
    if (value > 127)
        return 127;
    return value;
}

[[nodiscard]] inline std::size_t
metric_active_length(const pattern& p) noexcept
{
    return std::min<std::size_t>(p.length, steps_per_pattern);
}

} // namespace detail

/// Compute the deterministic metrics of a score.
[[nodiscard]] inline metrics
compute(const score& s)
{
    metrics out;

    std::array<std::int64_t, steps_per_pattern> column_counts {};
    std::array<double, steps_per_pattern> column_gain {};
    std::array<std::array<std::int32_t, track_count>, steps_per_pattern>
        column_notes {};
    std::array<std::size_t, steps_per_pattern> column_sizes {};

    std::int64_t gated { 0 };
    std::int64_t active { 0 };
    bool any_note { false };
    std::int32_t lowest { 127 };
    std::int32_t highest { 0 };

    for (std::size_t track_index { 0 }; track_index < track_count;
         ++track_index)
    {
        const auto& track { s.tracks[track_index] };

        if (track.mute || track.pattern >= pattern_count)
            continue;

        const auto& p { s.patterns[track.pattern] };
        const auto length { detail::metric_active_length(p) };
        active += static_cast<std::int64_t>(length);

        for (std::size_t column { 0 }; column < length; ++column)
        {
            const auto& st { p.steps[column] };

            if (!detail::sounding(st))
                continue;

            const auto note {
                detail::effective_note(st.note, track.transpose)
            };

            ++gated;
            ++column_counts[column];
            column_gain[column] += (static_cast<double>(track.gain) / 255.0) *
                                   (static_cast<double>(st.velocity) / 127.0);
            column_notes[column][column_sizes[column]] = note;
            ++column_sizes[column];

            if (!any_note || note < lowest)
                lowest = note;
            if (!any_note || note > highest)
                highest = note;
            any_note = true;
        }
    }

    if (active > 0)
        out.event_density =
            static_cast<double>(gated) / static_cast<double>(active);

    for (std::size_t column { 0 }; column < steps_per_pattern; ++column)
    {
        out.simultaneity = std::max(out.simultaneity, column_counts[column]);
        out.clipping_risk = std::max(out.clipping_risk, column_gain[column]);

        for (std::size_t i { 0 }; i < column_sizes[column]; ++i)
            for (std::size_t j { 0 }; j < i; ++j)
                if (column_notes[column][j] == column_notes[column][i])
                {
                    ++out.duplicate_notes;
                    break;
                }
    }

    if (any_note)
        out.register_spread = static_cast<std::int64_t>(highest - lowest);

    return out;
}

/// Compute the deterministic metrics of one rendered tick.
[[nodiscard]] inline metrics
compute(const event_batch& batch, const voice_state& voices)
{
    metrics out;

    struct onset
    {
        std::size_t event_index { 0 };
        std::int16_t offset { 0 };
        std::int32_t note { 0 };
        std::uint8_t velocity { 0 };
        std::uint8_t track { 0 };
    };

    std::array<onset, event_capacity> onsets {};
    std::size_t onset_count { 0 };
    const auto events { std::min<std::size_t>(batch.count, event_capacity) };

    bool any_note { false };
    std::int32_t lowest { 127 };
    std::int32_t highest { 0 };

    for (std::size_t i { 0 }; i < events; ++i)
    {
        const auto& event { batch.events[i] };

        if (!event.on || event.velocity == 0)
            continue;

        onsets[onset_count] =
            onset { i, event.offset, event.note, event.velocity, event.track };
        ++onset_count;

        if (!any_note || event.note < lowest)
            lowest = event.note;
        if (!any_note || event.note > highest)
            highest = event.note;
        any_note = true;
    }

    if (onset_count > 0)
        out.event_density = static_cast<double>(onset_count) /
                            static_cast<double>(event_capacity);

    for (std::size_t i { 0 }; i < onset_count; ++i)
    {
        const auto& a { onsets[i] };
        std::int64_t simultaneous { 0 };
        double gain { 0.0 };

        for (std::size_t j { 0 }; j < onset_count; ++j)
        {
            const auto& b { onsets[j] };

            if (b.offset != a.offset)
                continue;

            ++simultaneous;
            gain += static_cast<double>(b.velocity) / 127.0;

            if (j < i && b.note == a.note)
                ++out.duplicate_notes;
        }

        out.simultaneity = std::max(out.simultaneity, simultaneous);
        out.clipping_risk = std::max(out.clipping_risk, gain);
    }

    for (std::size_t i { 0 }; i < onset_count; ++i)
    {
        const auto& a { onsets[i] };
        bool released { false };

        for (std::size_t j { a.event_index + 1 }; j < events; ++j)
        {
            const auto& event { batch.events[j] };

            if (!event.on && event.track == a.track)
            {
                released = true;
                break;
            }
        }

        if (!released)
            ++out.unresolved_sustains;
    }

    if (any_note)
        out.register_spread = static_cast<std::int64_t>(highest - lowest);

    for (const auto& voice : voices.slots)
        if (voice.active)
            ++out.voice_pressure;

    return out;
}

} // namespace jam
