#include "clockr/replay/replayer.hpp"

#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/release_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace clockr::replay
{

namespace
{

class vector_trace_sink final : public kernel::trace_sink
{
public:
    explicit vector_trace_sink(std::vector<kernel::trace_event>& events) :
        m_events { events }
    {}

    void
    on_event(const kernel::trace_event& event) override
    {
        m_events.push_back(event);
    }

private:
    std::vector<kernel::trace_event>& m_events;
};

void
append_fidelity(fidelity& state, std::string reason, std::uint64_t missing)
{
    state.complete = false;
    state.missing_entries += missing;

    if (reason.empty())
        return;

    if (!state.reason.empty())
        state.reason += "; ";

    state.reason += reason;
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

run
perform(
    const scenario& source,
    branch_id branch,
    const std::vector<const scenario_entry*>& schedule,
    program_factory& factory,
    const codec_registry& codecs,
    const replay_options& options
)
{
    auto program { factory.make(source, options.program_options) };

    run result;
    result.run = source.run;
    result.branch = branch;
    result.fidelity = source.fidelity;

    const auto output_count { program->descriptor().output_ids.size() };
    std::array<std::uint64_t, kernel::clock_tag_count> ordinals {};

    vector_trace_sink sink { result.trace };
    kernel::release_engine releasing;
    std::unique_ptr<kernel::debug_engine> tracing;
    kernel::engine* engine { &releasing };

    if (options.capture_trace)
    {
        tracing = std::make_unique<kernel::debug_engine>(sink);
        engine = tracing.get();
    }

    bool stop_loop { false };

    for (const auto* entry : schedule)
    {
        switch (entry->kind)
        {
            case entry_kind::init:
            case entry_kind::mark:
                break;

            case entry_kind::set_input:
            {
                kernel::value decoded;
                std::string error;

                if (!decode_value(
                        entry->value,
                        *program,
                        entry->clock,
                        codecs,
                        decoded,
                        error
                    ))
                {
                    append_fidelity(
                        result.fidelity,
                        "replay input '" + entry->port + "': " + error,
                        1
                    );
                    break;
                }

                program->set(entry->port, decoded);
                kernel::release(decoded);
                break;
            }

            case entry_kind::step_region:
            {
                if (options.max_steps != 0 &&
                    result.steps.size() >= options.max_steps)
                {
                    append_fidelity(
                        result.fidelity, "replay stopped at max_steps", 0
                    );
                    stop_loop = true;
                    break;
                }

                const auto index { kernel::clock_tag_index(entry->clock) };
                const auto attempted {
                    program->region(entry->clock)
                        .published_epoch.load(std::memory_order_relaxed) +
                    1
                };

                step_record record;
                record.step =
                    region_step_id { entry->clock, ordinals[index]++ };
                record.epoch = attempted;

                try
                {
                    engine->step(*program, entry->clock);

                    if (options.capture_outputs)
                    {
                        record.outputs.reserve(output_count);

                        for (std::size_t i { 0 }; i < output_count; ++i)
                        {
                            encoded_value encoded;
                            std::string error;

                            if (!encode_value(
                                    program->get(i), codecs, encoded, error
                                ))
                            {
                                append_fidelity(
                                    result.fidelity,
                                    "replay output " + std::to_string(i) +
                                        ": " + error,
                                    1
                                );
                                encoded = encoded_value {};
                            }

                            record.outputs.push_back(std::move(encoded));
                        }
                    }

                    result.steps.push_back(std::move(record));
                }
                catch (...)
                {
                    record.threw = true;
                    result.steps.push_back(std::move(record));
                    result.aborted = true;
                    append_fidelity(result.fidelity, "replay step threw", 0);

                    if (options.stop_on_throw)
                        stop_loop = true;
                }

                break;
            }

            default:
                break;
        }

        if (stop_loop)
            break;
    }

    // Trace events borrow program-owned cells; a run outlives its program, so
    // reduce those payloads to their unit marker. Output capture is the
    // lossless path for user values.
    for (auto& event : result.trace)
        if (std::holds_alternative<kernel::user_ref>(event.payload))
            event.payload = kernel::value { std::monostate {} };

    return result;
}

} // namespace

run
replayer::replay(
    const scenario& source,
    program_factory& factory,
    const codec_registry& codecs,
    replay_options options
)
{
    std::vector<const scenario_entry*> schedule;
    schedule.reserve(source.entries.size());

    for (const auto& entry : source.entries)
        schedule.push_back(&entry);

    return perform(source, source.branch, schedule, factory, codecs, options);
}

run
replayer::replay_branch(
    const scenario& base,
    const branch_edit& edit,
    branch_id branch,
    program_factory& factory,
    const codec_registry& codecs,
    replay_options options
)
{
    std::vector<const scenario_entry*> schedule;

    for (const auto& entry : base.entries)
        if (entry.id <= edit.cut)
            schedule.push_back(&entry);

    for (const auto& entry : edit.inserted)
        schedule.push_back(&entry);

    for (const auto& entry : base.entries)
    {
        if (entry.id <= edit.cut || is_removed(edit, entry.id))
            continue;

        if (const auto* replacement { replacement_for(edit, entry.id) })
            schedule.push_back(replacement);
        else
            schedule.push_back(&entry);
    }

    return perform(base, branch, schedule, factory, codecs, options);
}

} // namespace clockr::replay
