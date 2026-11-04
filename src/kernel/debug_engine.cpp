#include "clockr/kernel/debug_engine.hpp"

namespace clockr::kernel
{

debug_engine::debug_engine(trace_sink& sink) :
    m_sink { sink }
{}

debug_engine::~debug_engine()
{
    clear_failure();
}

void
debug_engine::clear_failure()
{
    if (!m_last_failure.has_value())
        return;

    for (const auto& value : m_last_failure->fan_in)
        kernel::release(value);

    m_last_failure.reset();
}

namespace
{
    std::uint64_t
    elapsed_ns(const std::chrono::steady_clock::time_point& start)
    {
        const auto elapsed
        {
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
        };

        return static_cast<std::uint64_t>(elapsed.count());
    }
} // namespace

void
debug_engine::on_region_step_begin(kernel::clock_tag clock, conc::epoch_id epoch)
{
    m_clock = clock;
    m_epoch = epoch;
    m_region_start = std::chrono::steady_clock::now();
    m_sink.on_event({ trace_event_kind::region_step_begin, m_clock, m_epoch, 0, m_sequence++, {}, 0 });
}

void
debug_engine::on_region_step_end(kernel::clock_tag clock, conc::epoch_id epoch, bool threw)
{
    m_clock = clock;
    m_epoch = epoch;
    // Whole-tick time, happy path and unwind path alike: how long the region
    // took end to end, the coarse counterpart to the per-node durations.
    const auto duration_ns { elapsed_ns(m_region_start) };
    m_sink.on_event({ trace_event_kind::region_step_end, m_clock, m_epoch, 0, m_sequence++, kernel::value { threw }, duration_ns });
}

void
debug_engine::on_node_tick_begin(kernel::node_id)
{
    m_eval_start = std::chrono::steady_clock::now();
}

void
debug_engine::on_node_tick_end(kernel::node_id id, kernel::value value)
{
    const auto duration_ns { elapsed_ns(m_eval_start) };
    m_sink.on_event({ trace_event_kind::node_tick_end, m_clock, m_epoch, id, m_sequence++, value, duration_ns });
}

void
debug_engine::on_node_tick_throw(kernel::node_id id, std::span<const kernel::value> fan_in)
{
    // The captured fan-in outlives the throwing tick (the host reads it after
    // the exception), so it takes its own references.
    clear_failure();

    eval_failure failure { id, {} };
    failure.fan_in.reserve(fan_in.size());

    for (const auto& value : fan_in)
    {
        failure.fan_in.push_back(value);
        kernel::retain(value);
    }

    m_last_failure = std::move(failure);
    m_sink.on_event({ trace_event_kind::node_tick_throw, m_clock, m_epoch, id, m_sequence++, {}, elapsed_ns(m_eval_start) });
}

} // namespace clockr::kernel
