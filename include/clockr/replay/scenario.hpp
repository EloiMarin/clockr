#pragma once

/// @file
/// Immutable scenario journals and branch edits.

#include "clockr/replay/identity.hpp"
#include "clockr/replay/value_codec.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clockr::replay
{

/// One boundary operation in a scenario.
struct scenario_entry
{
    entry_id id {};
    entry_kind kind { entry_kind::mark };
    kernel::clock_tag clock { kernel::clock_tag::audio };
    std::uint64_t epoch_group { 0 };
    std::string port;
    encoded_value value;
    bool is_event { false };
    std::string note;
    bool threw { false };
};

/// Completeness accounting for a journal. An incomplete scenario may replay,
/// but its result must never be labelled exact.
struct fidelity
{
    bool complete { true };
    std::uint64_t missing_entries { 0 };
    std::string reason;
};

/// An immutable ordered boundary log.
struct scenario
{
    run_id run {};
    branch_id branch {};
    std::optional<run_id> parent;
    entry_id base_entry {};
    std::vector<scenario_entry> entries;
    fidelity fidelity;
};

/// Builds a scenario, assigning monotonic entry ids and epoch groups.
///
/// Sets in one `epoch_group` form an unordered input map; a step closes the
/// group.
class scenario_builder
{
public:
    explicit scenario_builder(
        run_id run,
        branch_id branch,
        std::optional<run_id> parent = {}
    );

    /// Append an opaque application annotation.
    void
    mark(std::string note);

    /// Open a new input group for `clock`. Calling it while a group is open
    /// starts a fresh group; the previous entries keep their group id.
    void
    begin_epoch(kernel::clock_tag clock);

    /// Record an input set in the current group.
    void
    set_input(std::string_view port, encoded_value value, bool is_event);

    /// Record a region step and close the current input group. Returns the
    /// entry id so a caller can annotate the attempt (see `mark_threw`).
    entry_id
    step_region(kernel::clock_tag clock);

    /// Record the initialization boundary.
    entry_id
    mark_initialized(kernel::clock_tag clock);

    /// Mark the journal incomplete (a late start, an unencodable value, ...).
    void
    mark_incomplete(std::string reason);

    /// Mark the journal incomplete and account for missing entries.
    void
    mark_missing(std::uint64_t missing, std::string reason);

    /// Record that the step entry `id` threw.
    void
    mark_threw(entry_id id);

    /// The scenario as recorded so far, without freezing it.
    const scenario&
    peek() const noexcept
    {
        return m_scenario;
    }

    /// Freeze the ids and return the journal.
    scenario
    finish();

private:
    void
    require_writable() const;

    void
    ensure_group();

    entry_id
    next_id();

    void
    append_reason(const std::string& reason);

    scenario m_scenario;
    std::uint64_t m_next_entry { 1 };
    std::uint64_t m_next_epoch_group { 1 };
    std::uint64_t m_epoch_group { 0 };
    kernel::clock_tag m_clock { kernel::clock_tag::audio };
    bool m_group_open { false };
    bool m_finished { false };
};

/// An edit relative to a cut entry in a base scenario.
struct branch_edit
{
    entry_id cut {};
    std::vector<scenario_entry> inserted;
    std::vector<entry_id> removed;
    std::vector<std::pair<entry_id, scenario_entry>> replaced;
};

/// Derive a branch scenario from `base`. The base is never mutated.
///
/// Unchanged prefix and suffix entries keep their ids; inserted and replacement
/// entries receive fresh ids above the base maximum.
scenario
apply_branch(
    const scenario& base,
    const branch_edit& edit,
    branch_id new_branch
);

} // namespace clockr::replay
