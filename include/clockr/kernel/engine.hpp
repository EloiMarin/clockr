#pragma once

#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/node.hpp"
#include "clockr/kernel/program.hpp"

#include <cassert>

namespace clockr::kernel
{

namespace detail
{

/// Enters a region's read window for the duration of its tick and leaves it at
/// scope exit.
///
/// While a region is announced, cells retired with an epoch at or before the
/// announcement are withheld from reclamation. That closes the one race a
/// lock-free `latest` reader has: between reading a slot's pointer and taking
/// its reference, a producer could displace and release the value — the
/// announcement keeps the memory from being reused until this read window
/// closes.
class step_grace_guard
{
public:
    explicit step_grace_guard(kernel::value_home* home) noexcept :
        m_home { home }
    {
        if (m_home == nullptr || m_home->announce == nullptr)
            return;

        const auto epoch { m_home->grace->epoch.fetch_add(1, std::memory_order_seq_cst) + 1 };
        m_home->announce->store(epoch, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    ~step_grace_guard()
    {
        if (m_home != nullptr && m_home->announce != nullptr)
            m_home->announce->store(0, std::memory_order_seq_cst);
    }

    step_grace_guard(const step_grace_guard&) = delete;
    step_grace_guard& operator=(const step_grace_guard&) = delete;

private:
    kernel::value_home* m_home;
};

/// Releases every user-cell reference a tick's value array holds, at scope
/// exit and on the unwind path. The engine's ownership rule is uniform: every
/// slot in `values` owns exactly one reference at all times — input ports and
/// links retain into it, produced values arrive owned — so one sweep
/// balances the whole tick.
class value_release_guard
{
public:
    explicit value_release_guard(std::span<kernel::value> values) noexcept :
        m_values { values }
    {}

    value_release_guard(const value_release_guard&) = delete;
    value_release_guard& operator=(const value_release_guard&) = delete;

    ~value_release_guard()
    {
        for (const auto& value : m_values)
            kernel::release(value);
    }

private:
    std::span<kernel::value> m_values;
};

#ifndef NDEBUG
/// Debug-only: asserts that no two threads are inside the same region's tick
/// at once. A region may move between threads *between* ticks (a thread pool
/// ticking the `worker` clock, say), so only overlap is a bug — and this
/// catches it before the state corruption it would cause.
class region_step_guard
{
public:
    explicit region_step_guard(kernel::program::region_runtime& region) noexcept :
        m_region { region }
    {
        assert(!m_region.in_step.exchange(true, std::memory_order_acq_rel)
               && "region ticked concurrently from two threads");
    }

    ~region_step_guard()
    {
        m_region.in_step.store(false, std::memory_order_release);
    }

    region_step_guard(const region_step_guard&) = delete;
    region_step_guard& operator=(const region_step_guard&) = delete;

private:
    kernel::program::region_runtime& m_region;
};
#endif

} // namespace detail

/// The tick-boundary extension point (a step *hook*, not a trace observer) a
/// host can attach to any `engine`.
///
/// Why it exists: a host sometimes needs to act *between* ticks — the live
/// debugger applies queued write-back commands at every boundary — and the
/// wrong way to offer that is a per-call-site wrapper (`step(program, clock)`
/// plus drain/finish around it). That couples every tick loop to the debug
/// tool and turns "build with the debugger or without it, or step a
/// `debug_engine` instead of a `release_engine`" into an edit at each call
/// site. Attaching one hook at construction keeps the loop exactly
/// `engine.step(program, clock)` for every engine type, which is the whole
/// point: the engine is chosen polymorphically at construction, not in the
/// loop body.
///
/// Threading: both hooks run on the ticking thread, inside `engine::step`.
/// Attachment follows the same quiescence contract as `program::set`: set it
/// while the engine is not stepping (normally at construction), and the hook
/// must outlive the attachment (the engine does not own it).
///
/// Hooks must be cheap and must not throw: `before_step` runs before the
/// tick's own guards, and `after_step` runs on the unwind path of a throwing
/// tick, so a throw from a hook would compound the in-flight exception. The
/// debugger's `command_hook` honours this; the engine deliberately does
/// not wrap the calls in try/catch (that would cost the step path).
class step_hook
{
public:
    virtual ~step_hook() = default;

    /// Called as the first statement of `engine::step`, on the ticking thread,
    /// before the affinity guard, before `program.reclaim`, and before any
    /// region state is touched. A write to the program here is an ordinary
    /// between-tick set: the tick being stepped consumes it (the value is set
    /// *before* evaluation, never during it), which is exactly where injection
    /// belongs for a clocked-synchronous network.
    virtual void
    before_step(kernel::program& program, kernel::clock_tag clock) = 0;

    /// Called exactly once after the tick, on the ticking thread.
    ///
    /// Happy path: after the tick's epoch is published and after
    /// `on_region_step_end(clock, epoch, false)`. Throwing path: after
    /// `on_region_step_end(clock, epoch, true)`, before the exception
    /// propagates, with `epoch` = the attempted tick's epoch (the same epoch
    /// the trace's `region_step_end` carries with `threw = true`) and
    /// `threw = true`. A throwing tick publishes no epoch, so the hook is the
    /// only place that learns which tick was attempted.
    virtual void
    after_step(
        kernel::program& program,
        kernel::clock_tag clock,
        conc::epoch_id epoch,
        bool threw
    ) = 0;
};

/// Executes one region of a `program` per call. An engine holds no graph
/// state: `step(program, clock)` drains the region's inbound links at one
/// producer cut, evaluates its nodes in topological order, settles delay
/// state and outputs, then publishes one epoch. One region may move between
/// threads over time, but it must never be stepped concurrently.
///
/// The step itself is not virtual: `debug_engine`/`release_engine` differ only
/// in the protected trace hooks they override, so an attached
/// `step_hook` brackets both identically — a host can switch engine types
/// (or drop the debugger) without touching its tick loop.
class engine
{
public:
    virtual ~engine() = default;

    /// Tick `program`'s region `clock` once on the calling thread. Bracketed
    /// by the attached step hook, if any: `before_step` first, then the
    /// region drain/evaluate/publish, then `after_step` once.
    void
    step(kernel::program&, kernel::clock_tag);

    /// Attach a tick-boundary hook, or detach with `nullptr`. Non-owning: the
    /// hook must outlive the attachment, and attachment is a quiescent act
    /// (the same construction-time discipline as picking the engine type).
    /// With nothing attached a step pays one predictable branch, no virtual
    /// call, no allocation.
    void
    set_step_hook(step_hook* hook) noexcept
    {
        m_step_hook = hook;
    }

    /// The attached hook, or null. Read-only accessor for hosts and tests.
    step_hook*
    get_step_hook() const noexcept
    {
        return m_step_hook;
    }

protected:
    // Fires once per region tick, before any node is evaluated. `epoch` is the
    // region's published_epoch counter (the id the links stamp), so
    // (thread, epoch) names the tick. Both region hooks are no-ops by default
    // so release evaluation pays nothing for them.
    virtual void
    on_region_step_begin(kernel::clock_tag, conc::epoch_id epoch);

    // Fires once per region tick, after outputs and delay state are settled.
    // `threw` is true when a node's tick_fn threw, in which case this fires
    // from the unwind path before the exception propagates out of the tick.
    virtual void
    on_region_step_end(kernel::clock_tag, conc::epoch_id epoch, bool threw);

    virtual void
    on_node_tick_begin(kernel::node_id);

    virtual void
    on_node_tick_end(kernel::node_id, kernel::value);

    // Fires when a node's tick_fn throws, before the exception propagates out
    // of the tick.
    virtual void
    on_node_tick_throw(kernel::node_id, std::span<const kernel::value> fan_in);

private:
    // Port receives (the drain phase).
    void
    drain(kernel::program&, kernel::program::region_runtime&, std::span<kernel::value> values);

    // Evaluates this region's nodes into `values`.
    template<kernel::graph_c graph_t>
    void
    evaluate_nodes(
        const graph_t&,
        kernel::program::region_runtime&,
        std::span<kernel::value> values
    );

    // Update outputs and delay states.
    template<kernel::graph_c graph_t>
    void
    settle(
        const graph_t&,
        kernel::program::region_runtime&,
        std::span<const kernel::value> step_values
    );

    // Port sends for one producer epoch (the publish phase; computed once
    // channels of the tick share it; published with release at tick end).
    void
    publish(
        kernel::program&,
        const kernel::descriptor_view&,
        kernel::program::region_runtime&,
        std::span<kernel::value> values,
        conc::epoch_id epoch
    );

    // Consumed event inputs revert to disengaged.
    void
    clear(kernel::program::region_runtime& region_runtime);

    // Tick-boundary hooks; null when nothing is attached (the shared fast
    // path). Deliberately a plain pointer: attachment is a quiescent,
    // construction-time act, so the step path needs no atomics.
    step_hook* m_step_hook { nullptr };
};

} // namespace clockr::kernel

// ############### IMPLEMENTATION ################
namespace clockr::kernel
{

template<kernel::graph_c graph_t>
void
engine::evaluate_nodes(
    const graph_t& graph,
    kernel::program::region_runtime& region,
    std::span<kernel::value> values
)
{
    const kernel::tick_context ctx { region.home };

    for (const auto id : region.evaluation_order)
    {
        const auto& node { at(graph, id) };

        on_node_tick_begin(id);

        switch (kind(node))
        {
            case kernel::node_kind::input:
            {
                assert(id < region.input_slot_of.size());
                assert(region.input_slot_of[id] != kernel::program::region_runtime::no_slot);

                // The tick's slot takes its own reference (dropped by the
                // tick-end guard); the port keeps its own.
                const auto& input { region.inputs[region.input_slot_of[id]] };
                values[id] = input;
                kernel::retain(input);
                break;
            }
            case kernel::node_kind::output:
                [[fallthrough]];
            case kernel::node_kind::internal:
            {
                std::vector<kernel::value, mem::pool_allocator<kernel::value>> fan_values { mem::pool_allocator<kernel::value> { region.home->pool } };
                std::span<const kernel::node_id> node_fan_in { fan_in(graph, id) };

                fan_values.reserve(node_fan_in.size());

                for (const auto node_id : node_fan_in)
                    fan_values.push_back(values[node_id]);

                kernel::value node_output;

                try
                {
                    node_output = tick(node, ctx, fan_values);
                }
                catch (...)
                {
                    on_node_tick_throw(id, fan_values);
                    throw;
                }

                values[id] = node_output;
                break;
            }
            case kernel::node_kind::delay:
            {
                assert(id < region.delay_slot_of.size());
                assert(region.delay_slot_of[id] != kernel::program::region_runtime::no_slot);
                kernel::delay_state& node_state { region.delay_states[region.delay_slot_of[id]] };

                if (node_state.has_value())
                {
                    // Borrow the state for this tick, with the tick's own
                    // reference (the slot keeps its reference across ticks).
                    values[id] = node_state.value();
                    kernel::retain(values[id]);
                }
                else
                {
                    std::vector<kernel::value, mem::pool_allocator<kernel::value>> fan_values { mem::pool_allocator<kernel::value> { region.home->pool } };
                    std::span<const kernel::node_id> node_fan_in { fan_in(graph, id) };

                    fan_values.reserve(node_fan_in.size());

                    for (const auto node_index : node_fan_in)
                        fan_values.push_back(values[node_index]);

                    try
                    {
                        values[id] = tick(node, ctx, fan_values);
                    }
                    catch (...)
                    {
                        on_node_tick_throw(id, fan_values);
                        throw;
                    }
                }
                break;
            }
            default:
                break;
        }

        on_node_tick_end(id, values[id]);
    }
}

template<kernel::graph_c graph_t>
void
engine::settle(
    const graph_t& graph,
    kernel::program::region_runtime& region,
    std::span<const kernel::value> step_values
)
{
    for (const auto id : region.evaluation_order)
    {
        const auto& node { at(graph, id) };
        const auto& node_output_value { step_values[id] };

        switch (kind(node))
        {
            case kernel::node_kind::input:
                break;
            case kernel::node_kind::output:
                assert(id < region.output_slot_of.size());
                assert(region.output_slot_of[id] != kernel::program::region_runtime::no_slot);
                // The output owns an independent reference across ticks; the
                // previous output is released.
                kernel::retain_assign(region.outputs[region.output_slot_of[id]], node_output_value);
                break;
            case kernel::node_kind::internal:
                break;
            case kernel::node_kind::delay:
            {
                assert(id < region.delay_slot_of.size());
                assert(region.delay_slot_of[id] != kernel::program::region_runtime::no_slot);
                kernel::delay_state& node_state { region.delay_states[region.delay_slot_of[id]] };

                std::vector<kernel::value, mem::pool_allocator<kernel::value>> fan_values { mem::pool_allocator<kernel::value> { region.home->pool } };
                std::span<const kernel::node_id> node_fan_in { fan_in(graph, id) };

                fan_values.reserve(node_fan_in.size());

                for (const auto in : node_fan_in)
                    fan_values.push_back(step_values[in]);

                if (!fan_values.empty())
                    kernel::retain_assign(node_state, fan_values[0]);
                break;
            }
            default:
                break;
        }
    }
}

} // namespace clockr::kernel
