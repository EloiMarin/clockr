#include "clockr/replay/scenario.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace clockr::replay
{

scenario_builder::scenario_builder(
    run_id run,
    branch_id branch,
    std::optional<run_id> parent
)
{
    m_scenario.run = run;
    m_scenario.branch = branch;
    m_scenario.parent = parent;
}

void
scenario_builder::require_writable() const
{
    if (m_finished)
        throw std::logic_error { "scenario is frozen" };
}

void
scenario_builder::append_reason(const std::string& reason)
{
    if (reason.empty())
        return;

    if (!m_scenario.fidelity.reason.empty())
        m_scenario.fidelity.reason += "; ";

    m_scenario.fidelity.reason += reason;
}

entry_id
scenario_builder::next_id()
{
    return entry_id { m_next_entry++ };
}

void
scenario_builder::ensure_group()
{
    if (m_group_open)
        return;

    m_epoch_group = m_next_epoch_group++;
    m_group_open = true;
}

void
scenario_builder::mark(std::string note)
{
    require_writable();

    scenario_entry entry;
    entry.id = next_id();
    entry.kind = entry_kind::mark;
    entry.clock = m_clock;
    entry.epoch_group = m_group_open ? m_epoch_group : 0;
    entry.note = std::move(note);

    m_scenario.entries.push_back(std::move(entry));
}

void
scenario_builder::begin_epoch(kernel::clock_tag clock)
{
    require_writable();

    m_clock = clock;
    m_epoch_group = m_next_epoch_group++;
    m_group_open = true;
}

void
scenario_builder::set_input(
    std::string_view port,
    encoded_value value,
    bool is_event
)
{
    require_writable();
    ensure_group();

    scenario_entry entry;
    entry.id = next_id();
    entry.kind = entry_kind::set_input;
    entry.clock = m_clock;
    entry.epoch_group = m_epoch_group;
    entry.port = std::string { port };
    entry.value = std::move(value);
    entry.is_event = is_event;

    m_scenario.entries.push_back(std::move(entry));
}

entry_id
scenario_builder::step_region(kernel::clock_tag clock)
{
    require_writable();

    if (!m_group_open)
        m_epoch_group = m_next_epoch_group++;

    scenario_entry entry;
    entry.id = next_id();
    entry.kind = entry_kind::step_region;
    entry.clock = clock;
    entry.epoch_group = m_epoch_group;

    m_scenario.entries.push_back(std::move(entry));
    m_group_open = false;

    return entry.id;
}

entry_id
scenario_builder::mark_initialized(kernel::clock_tag clock)
{
    require_writable();

    scenario_entry entry;
    entry.id = next_id();
    entry.kind = entry_kind::init;
    entry.clock = clock;
    entry.epoch_group = m_group_open ? m_epoch_group : 0;

    m_scenario.entries.push_back(std::move(entry));

    return entry.id;
}

void
scenario_builder::mark_incomplete(std::string reason)
{
    require_writable();

    m_scenario.fidelity.complete = false;
    append_reason(reason);
}

void
scenario_builder::mark_missing(std::uint64_t missing, std::string reason)
{
    require_writable();

    m_scenario.fidelity.complete = false;
    m_scenario.fidelity.missing_entries += missing;
    append_reason(reason);
}

void
scenario_builder::mark_threw(entry_id id)
{
    require_writable();

    const auto it { std::find_if(
        m_scenario.entries.rbegin(),
        m_scenario.entries.rend(),
        [id](const scenario_entry& entry) {
            return entry.id == id;
        }
    ) };

    if (it != m_scenario.entries.rend())
        it->threw = true;
}

scenario
scenario_builder::finish()
{
    m_finished = true;
    return m_scenario;
}

namespace
{

void
set_fresh_id(scenario_entry& entry, std::uint64_t& next)
{
    entry.id = entry_id { next++ };
}

bool
is_removed(const branch_edit& edit, entry_id id)
{
    return std::find(edit.removed.begin(), edit.removed.end(), id) !=
           edit.removed.end();
}

const scenario_entry*
replacement_for(const branch_edit& edit, entry_id id)
{
    for (const auto& [replace_id, entry] : edit.replaced)
        if (replace_id == id)
            return &entry;

    return nullptr;
}

} // namespace

scenario
apply_branch(
    const scenario& base,
    const branch_edit& edit,
    branch_id new_branch
)
{
    scenario result;
    result.run = base.run;
    result.branch = new_branch;
    result.parent = base.run;
    result.base_entry = edit.cut;
    result.fidelity = base.fidelity;

    std::uint64_t next { 1 };

    for (const auto& entry : base.entries)
        if (entry.id.value >= next)
            next = entry.id.value + 1;

    for (const auto& entry : base.entries)
        if (entry.id <= edit.cut)
            result.entries.push_back(entry);

    for (auto entry : edit.inserted)
    {
        set_fresh_id(entry, next);
        result.entries.push_back(std::move(entry));
    }

    for (const auto& entry : base.entries)
    {
        if (entry.id <= edit.cut || is_removed(edit, entry.id))
            continue;

        if (const auto* replacement { replacement_for(edit, entry.id) })
        {
            auto edited { *replacement };
            set_fresh_id(edited, next);
            result.entries.push_back(std::move(edited));
        }
        else
        {
            result.entries.push_back(entry);
        }
    }

    return result;
}

} // namespace clockr::replay
