#pragma once

/// @file
/// Replay identity types: runs, branches, journal entries and logical region
/// steps.

#include "clockr/kernel/clock.hpp"

#include <compare>
#include <cstdint>

namespace clockr::replay
{

/// One recorded execution of a network. Entry ids are unique within a run.
struct run_id
{
    std::uint64_t value { 0 };

    friend auto
    operator<=>(const run_id&, const run_id&) = default;
};

/// One revision of a run. A branch names the run it was forked from.
struct branch_id
{
    std::uint64_t value { 0 };

    friend auto
    operator<=>(const branch_id&, const branch_id&) = default;
};

/// One boundary operation in a run's journal.
struct entry_id
{
    std::uint64_t value { 0 };

    friend auto
    operator<=>(const entry_id&, const entry_id&) = default;
};

/// A logical region tick: the region and its per-run ordinal. Epochs are
/// display metadata and never part of this identity.
struct region_step_id
{
    kernel::clock_tag clock { kernel::clock_tag::audio };
    std::uint64_t ordinal { 0 };

    friend auto
    operator<=>(const region_step_id&, const region_step_id&) = default;
};

/// The boundary operations a journal can contain.
enum class entry_kind : std::uint8_t
{
    init,
    set_input,
    step_region,
    mark,
};

} // namespace clockr::replay
