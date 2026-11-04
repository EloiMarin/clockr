#include "clockr/kernel/engine.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace clockr::kernel
{

void
engine::step(kernel::program& program, kernel::clock_tag clock)
{
    // First statement, before the affinity guard and before reclamation: the
    // hook's between-tick work (the debugger's queued-command drain) must see
    // the same state a host `set` would, and must run before this tick
    // consumes it. Nothing about the region has been touched yet.
    if (m_step_hook != nullptr)
        m_step_hook->before_step(program, clock);

    auto& region_runtime { program.region(clock) };

#ifndef NDEBUG
    // Catch a host ticking one region from two threads at once. Sequential
    // ticks from different threads are fine (thread pools); this is purely an
    // overlap check. Compiles out of release builds.
    const detail::region_step_guard affinity_guard { region_runtime };
#endif

    // Return the previous ticks' retired cells to the pool before this tick's
    // reads begin: work is bounded, and any cell a concurrent region could
    // still be reading is withheld by the grace check.
    program.reclaim(clock);

    // Announce this region's read window for the tick's duration — declared
    // before the release guard so it closes after the guard's releases.
    const detail::step_grace_guard grace_guard { region_runtime.home };

    const auto graph { program.descriptor() };
    std::vector<kernel::value, mem::pool_allocator<kernel::value>> values
    {
        size(graph),
        mem::pool_allocator<kernel::value> { region_runtime.home->pool }
    };

    // Every slot in `values` owns exactly one reference; this releases them on
    // scope exit, the throwing path included.
    const detail::value_release_guard release_values { values };

    // The new epoch under construction: old published + 1. All sends of
    // this tick stamp it; it becomes visible with the release store below,
    // after every channel of the epoch is staged — one atomic publication.
    const auto epoch
    {
        region_runtime.published_epoch.load(std::memory_order_relaxed) + 1
    };

    on_region_step_begin(clock, epoch);

    try
    {
        drain(program, region_runtime, values);
        evaluate_nodes(graph, region_runtime, values);
        settle(graph, region_runtime, values);
        publish(program, graph, region_runtime, values, epoch);
    }
    catch (...)
    {
        // A throwing node aborts the tick (the exception propagates); still
        // close the region bracket so consumers see the tick end, marked bad,
        // and tell the hook once with the attempted epoch (a throwing tick
        // publishes nothing, so this is the only report of which tick failed).
        on_region_step_end(clock, epoch, true);

        if (m_step_hook != nullptr)
            m_step_hook->after_step(program, clock, epoch, true);

        throw;
    }

    clear(region_runtime);

    region_runtime.published_epoch.store(epoch, std::memory_order_release);

    on_region_step_end(clock, epoch, false);

    if (m_step_hook != nullptr)
        m_step_hook->after_step(program, clock, epoch, false);
}

void
engine::on_region_step_begin(kernel::clock_tag, conc::epoch_id)
{}

void
engine::on_region_step_end(kernel::clock_tag, conc::epoch_id, bool)
{}

void
engine::on_node_tick_begin(kernel::node_id)
{}

void
engine::on_node_tick_end(kernel::node_id, kernel::value)
{}

void
engine::on_node_tick_throw(kernel::node_id, std::span<const kernel::value>)
{}

void
engine::drain(kernel::program& program, kernel::program::region_runtime& region_runtime, std::span<kernel::value> values)
{
    // One cut per producer region per consumer tick (§3.2 consistency):
    // snapshot each producer's published published_epoch once (acquire) and reuse
    // it for every channel from that producer, so a producer tick landing
    // mid-fill cannot tear the pair. A latest nullopt means the snapshot
    // was lapped by the producer's lead (ring overwrote it) — re-snapshot
    // fresh and retry the whole fill so the pair stays on one cut. Queued
    // nullopt is normal (empty or still-future head) and stages monostate
    // without retry.
    constexpr int max_attempts { 8 };

    for (int attempt { 0 }; attempt < max_attempts; ++attempt)
    {
        const auto published_audio
        {
            program.region(kernel::clock_tag::audio).published_epoch.load(std::memory_order_acquire)
        };
        const auto published_ui
        {
            program.region(kernel::clock_tag::ui).published_epoch.load(std::memory_order_acquire)
        };
        const auto published_worker
        {
            program.region(kernel::clock_tag::worker).published_epoch.load(std::memory_order_acquire)
        };

        auto published_for = [&](kernel::node_id from)
        {
            switch (program.clock_of(from))
            {
                case kernel::clock_tag::audio:
                    return published_audio;
                case kernel::clock_tag::ui:
                    return published_ui;
                case kernel::clock_tag::worker:
                    return published_worker;
                default:
                    return conc::epoch_id{ 0 };
            }
        };

        bool stale { false };

        for (const auto from : region_runtime.receives_latest)
        {
            if (const auto got { program.receive(from, published_for(from)) })
                kernel::transfer_assign(values[from], *got);
            else
            {
                stale = true;
                break;
            }
        }

        if (stale)
            continue;

        for (const auto from : region_runtime.receives_queued)
            kernel::transfer_assign(
                values[from],
                program.receive(from, published_for(from))
                    .value_or(kernel::value{ std::monostate{} }));

        return;
    }

    // Extremely contended fallback (producer lapping continuously): take the
    // newest on each latest channel so the tick still makes progress. This
    // may be newer than one cut, but only after repeated lapping — the
    // common case above already returns on a single cut. The counter makes
    // the relaxation observable (see program::lapped_fills).
    program.note_lapped_fill();

    constexpr auto newest { std::numeric_limits<conc::epoch_id>::max() };

    for (const auto from : region_runtime.receives_latest)
        kernel::transfer_assign(
            values[from],
            program.receive(from, newest).value_or(kernel::value{ std::monostate{} }));

    for (const auto from : region_runtime.receives_queued)
        kernel::transfer_assign(
            values[from],
            program.receive(from, newest).value_or(kernel::value{ std::monostate{} }));
}

void
engine::publish(
    kernel::program& program,
    const kernel::descriptor_view& graph,
    kernel::program::region_runtime& region_runtime,
    std::span<kernel::value> values,
    conc::epoch_id epoch
)
{
    const auto source_is_dirty
    {
        [&](kernel::node_id source_id)
        {
            if (kind(at(graph, source_id)) != kernel::node_kind::input)
                return true;

            assert(source_id < region_runtime.input_slot_of.size());

            const auto slot { region_runtime.input_slot_of[source_id] };

            assert(slot != kernel::program::region_runtime::no_slot);

            return region_runtime.input_dirty[slot] == std::byte{ 1 };
        }
    };

    for (const auto from : region_runtime.sends_queued)
        if (source_is_dirty(from))
            program.send_queued(from, values[from], epoch);

    for (const auto from : region_runtime.sends_latest)
        if (source_is_dirty(from))
            program.send_latest(from, values[from], epoch);
}

void
engine::clear(kernel::program::region_runtime& region_runtime)
{
    assert(region_runtime.input_is_event.size() == region_runtime.inputs.size());
    assert(region_runtime.input_dirty.size() == region_runtime.inputs.size());

    for (std::size_t i = 0; i < region_runtime.inputs.size(); ++i)
    {
        if (region_runtime.input_is_event[i] != std::byte{ 0 })
        {
            // Fire-and-forget inputs revert to disengaged: release the
            // reference the port held for the event.
            kernel::release(region_runtime.inputs[i]);
            region_runtime.inputs[i] = kernel::value{std::monostate{}};
        }

        region_runtime.input_dirty[i] = std::byte{ 0 };
    }
}

} // namespace clockr::kernel
