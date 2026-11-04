#include "recorder.hpp"

#include <chrono>
#include <utility>

namespace clockr::debug
{

namespace
{
    // One bounded queue per region, sized by the region count.
    template<std::size_t... I>
    std::array<conc::spsc<recorded_event>, kernel::clock_tag_count>
    make_queues(std::size_t capacity, std::index_sequence<I...>)
    {
        return { ((void)I, conc::spsc<recorded_event> { capacity })... };
    }
} // namespace

trace_recorder::trace_recorder(std::size_t capacity_per_thread) :
    m_queues { make_queues(capacity_per_thread, std::make_index_sequence<kernel::clock_tag_count>{}) }
{
    for (auto& dropped : m_dropped)
        dropped.store(0, std::memory_order_relaxed);
}

std::size_t
trace_recorder::slot_of(kernel::clock_tag tag) noexcept
{
    return kernel::clock_tag_index(tag);
}

trace_recorder::~trace_recorder()
{
    // Events the consumer never took still own their user-cell references
    // (see on_event); hand them back. Runs on the owner's thread, with the
    // program and its homes still alive.
    for (auto& queue : m_queues)
    {
        recorded_event event;

        while (queue.receive(event))
            kernel::release(event.event.payload);
    }
}

void
trace_recorder::on_event(const kernel::trace_event& event)
{
    if (!m_enabled.load(std::memory_order_relaxed))
        return;

    const auto slot { slot_of(event.clock) };

    const auto stamp
    {
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count())
    };

    // The queue's copy owns a reference: the ticking thread releases its own
    // references at tick end, and the consumer releases this one after
    // encoding. Retaining here is what keeps a traced struct alive long enough
    // to be rendered; the increment is a single atomic (safe on the audio
    // thread) and the release travels the home's retire queue.
    kernel::retain(event.payload);

    if (!m_queues[slot].send(recorded_event { event, stamp }))
    {
        kernel::release(event.payload);
        m_dropped[slot].fetch_add(1, std::memory_order_relaxed);
    }
}

void
trace_recorder::set_enabled(bool enabled) noexcept
{
    m_enabled.store(enabled, std::memory_order_relaxed);
}

bool
trace_recorder::enabled() const noexcept
{
    return m_enabled.load(std::memory_order_relaxed);
}

std::size_t
trace_recorder::drain(std::vector<recorded_event>& out, std::size_t max_events)
{
    std::size_t moved { 0 };

    // Round-robin across tags so one busy producer cannot starve the others.
    // Each individual queue is FIFO, so in-thread order is preserved exactly.
    bool progressed { true };

    while (moved < max_events && progressed)
    {
        progressed = false;

        for (auto& queue : m_queues)
        {
            recorded_event event;

            if (moved < max_events && queue.receive(event))
            {
                out.push_back(event);
                ++moved;
                progressed = true;
            }
        }
    }

    return moved;
}

std::array<std::uint64_t, 3>
trace_recorder::take_dropped() noexcept
{
    std::array<std::uint64_t, 3> dropped {};

    for (std::size_t slot { 0 }; slot < m_dropped.size(); ++slot)
        dropped[slot] = m_dropped[slot].exchange(0, std::memory_order_relaxed);

    return dropped;
}

void
trace_recorder::discard_pending()
{
    for (auto& queue : m_queues)
    {
        recorded_event event;

        while (queue.receive(event))
            kernel::release(event.event.payload);
    }
}

} // namespace clockr::debug
