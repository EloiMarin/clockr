#pragma once

#include "clockr/kernel/engine.hpp"
#include "clockr/kernel/trace.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace clockr::kernel
{

/// An `engine` that reports every evaluation step to a `trace_sink`.
///
/// Each node evaluation travels as a single message: `on_node_tick_begin`
/// only arms a timer, and `on_node_tick_end` / `on_node_tick_throw` emit the
/// outcome together with the measured evaluation time.
///
/// One instance traces one ticking clock: the sequence counter, the
/// region-tick context, and the timing state are plain members, so a single
/// `debug_engine` must not be ticked from concurrent threads. Construct one
/// per thread (audio/ui/worker) and share the sink.
class debug_engine final : public engine
{
public:
    struct eval_failure
    {
        kernel::node_id node;
        std::vector<kernel::value> fan_in;
    };

    explicit debug_engine(trace_sink& sink);

    ~debug_engine() override;

    debug_engine(const debug_engine&) = delete;
    debug_engine& operator=(const debug_engine&) = delete;

    bool
    has_failure() const noexcept
    {
        return m_last_failure.has_value();
    }

    // A failed node evaluation, as observed: which node threw, and the exact
    // fan-in values it choked on. The original exception keeps propagating.
    // Read it right after the throwing tick, on the same thread.
    const eval_failure&
    failure() const
    {
        return m_last_failure.value();
    }

private:
    // Drop the retained fan-in references of the last failure (the captured
    // vector owns one reference per value it holds).
    void
    clear_failure();

    void
    on_region_step_begin(kernel::clock_tag, conc::epoch_id epoch) override;

    void
    on_region_step_end(kernel::clock_tag, conc::epoch_id epoch, bool threw) override;

    void
    on_node_tick_begin(kernel::node_id) override;

    void
    on_node_tick_end(kernel::node_id, kernel::value) override;

    void
    on_node_tick_throw(kernel::node_id, std::span<const kernel::value> fan_in) override;

    trace_sink& m_sink;
    std::uint64_t m_sequence { 0 };
    kernel::clock_tag m_clock { kernel::clock_tag::audio };
    conc::epoch_id m_epoch { 0 };
    std::chrono::steady_clock::time_point m_region_start {};
    std::chrono::steady_clock::time_point m_eval_start {};
    std::optional<eval_failure> m_last_failure;
};

} // namespace clockr::kernel
