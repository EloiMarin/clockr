#pragma once

/// @file
/// Fresh-program replay of a scenario.

#include "clockr/replay/factory.hpp"
#include "clockr/replay/run.hpp"
#include "clockr/replay/scenario.hpp"
#include "clockr/replay/value_codec.hpp"

#include "clockr/kernel/program.hpp"

#include <cstddef>

namespace clockr::replay
{

/// Replay knobs.
struct replay_options
{
    bool capture_outputs { true };
    bool capture_trace { false };
    bool stop_on_throw { true };
    std::size_t max_steps { 0 };
    kernel::program_options program_options {};
};

/// Replays scenarios into fresh programs through the ordinary engine path.
class replayer
{
public:
    /// Replay `scenario` unchanged.
    run
    replay(
        const scenario& scenario,
        program_factory& factory,
        const codec_registry& codecs,
        replay_options options = {}
    );

    /// Replay the base prefix through `edit.cut`, apply the edit, then continue
    /// the suffix. The base scenario is never mutated.
    run
    replay_branch(
        const scenario& base,
        const branch_edit& edit,
        branch_id branch,
        program_factory& factory,
        const codec_registry& codecs,
        replay_options options = {}
    );
};

} // namespace clockr::replay
