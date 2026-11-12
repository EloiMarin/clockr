#pragma once

/// Shared test fixtures for the jam suites: one four-step pattern on track 0
/// (note 60 at step 0, note 64 at step 2) and nothing else sounding.

#include "jam/patch.hpp"
#include "jam/score.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace jam_test
{

inline jam::score
small_score()
{
    jam::score s {};

    auto& p { s.patterns[0] };
    p.length = 4;
    p.steps[0] = jam::step { 60, 100, true };
    p.steps[2] = jam::step { 64, 80, true };

    s.tracks[0] = jam::track { 0, 0, 255, false };
    s.bpm = 120;

    return s;
}

inline jam::score_patch
make_patch(std::initializer_list<jam::patch_step> ops)
{
    jam::score_patch patch {};

    for (const auto& op : ops)
    {
        if (patch.count >= jam::patch_capacity)
            break;

        patch.ops[static_cast<std::size_t>(patch.count)] = op;
        patch.count = static_cast<std::uint8_t>(patch.count + 1);
    }

    return patch;
}

} // namespace jam_test
