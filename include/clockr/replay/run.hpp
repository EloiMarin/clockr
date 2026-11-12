#pragma once

/// @file
/// One replay execution: logical steps, captured outputs and optional trace.

#include "clockr/replay/identity.hpp"
#include "clockr/replay/scenario.hpp"
#include "clockr/replay/value_codec.hpp"

#include "clockr/kernel/trace.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clockr::replay
{

/// One replayed region step.
struct step_record
{
    region_step_id step {};
    std::uint64_t epoch { 0 };
    bool threw { false };
    std::vector<encoded_value> outputs;
};

/// The result of replaying a scenario.
struct run
{
    run_id run {};
    branch_id branch {};
    std::vector<step_record> steps;
    std::vector<kernel::trace_event> trace;
    fidelity fidelity;
    bool aborted { false };
};

} // namespace clockr::replay
