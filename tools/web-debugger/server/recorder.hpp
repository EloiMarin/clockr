#pragma once

#include "clockr/conc/spsc.hpp"

#include "clockr/kernel/trace.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace clockr::debug
{

/// One event as captured, plus the producer thread's clock reading. Stamped
/// at capture time (not at drain time) so the UI can space ticks in real
/// time even though batches leave the server at a bounded rate.
struct recorded_event
{
    kernel::trace_event event;
    std::uint64_t stamp_ns;
};

/// A `trace_sink` that hands events from the ticking clock(s) to a consumer
/// thread without ever blocking or allocating on the producer path.
///
/// One bounded SPSC queue per thread tag (the enum is closed, so the queues
/// are a fixed array indexed by tag). A full queue drops the event and counts
/// it — an audio thread must never wait on a debugger. The server drains
/// periodically (see `Server`), so the queue only has to hold one drain
/// window's worth of events.
///
/// Threading: `on_event` is called from each producer thread (one queue per
/// tag, so each queue stays single-producer); `drain`/`take_dropped` are
/// called only from the consumer thread; `set_enabled` may be called from any
/// thread.
class trace_recorder final : public kernel::trace_sink
{
public:
    /// `capacity_per_thread` bounds each per-tag queue. Events beyond it in a
    /// drain window are dropped and counted (surfaced in the batch).
    explicit trace_recorder(std::size_t capacity_per_thread = 16 * 1024);

    ~trace_recorder() override;

    trace_recorder(const trace_recorder&) = delete;
    trace_recorder& operator=(const trace_recorder&) = delete;

    void
    on_event(const kernel::trace_event&) override;

    /// Gate capture. Disabled is the "idle debugger" state: events are
    /// dropped without a clock read or enqueue. Default enabled so a sink
    /// attached to a debug_engine records from the first tick; the server
    /// turns it on only while a client wants a live stream.
    void
    set_enabled(bool enabled) noexcept;

    bool
    enabled() const noexcept;

    /// Consumer side: move up to `max_events` queued events into `out`, in
    /// arrival order per thread tag. Returns the number moved.
    std::size_t
    drain(std::vector<recorded_event>& out, std::size_t max_events);

    /// Dropped-event counts accumulated since the last call, per thread tag,
    /// resetting them.
    std::array<std::uint64_t, kernel::clock_tag_count>
    take_dropped() noexcept;

    /// Consumer side: drop everything still queued, releasing the user-cell
    /// references the events own (same hand-back as the destructor, but the
    /// recorder stays usable). The server calls this when the live example
    /// changes so the new timeline never shows the old example's tail.
    /// Consumer-thread only, like drain.
    void
    discard_pending();

private:
    static
    std::size_t
    slot_of(kernel::clock_tag tag) noexcept;

    std::array<conc::spsc<recorded_event>, kernel::clock_tag_count> m_queues;
    std::array<std::atomic<std::uint64_t>, kernel::clock_tag_count> m_dropped {};
    std::atomic<bool> m_enabled { true };
};

} // namespace clockr::debug
