#pragma once

/// @file
/// The per-tick evaluation trace emitted by `debug_engine` (region and node
/// events with timings and values) and the `trace_sink` interface a consumer
/// implements. The live debugger is the in-tree consumer.

#include "clockr/conc/link.hpp"
#include "clockr/kernel/node.hpp"
#include "clockr/kernel/value.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>

namespace clockr::kernel
{

/// The kind of evaluation event in a trace stream.
enum class trace_event_kind : std::uint8_t
{
    region_step_begin,
    region_step_end,
    node_tick_begin,
    node_tick_end,
    node_tick_throw,
};

/// One step of an evaluation, as observed by a `debug_engine`.
///
/// A tick of one region brackets its node events:
///
///   region_step_begin, node_tick_end, ... region_step_end
///
/// (`node_tick_begin` exists as a hook for custom sinks, but `debug_engine`
/// does not emit it: each node evaluation travels as a single message.)
///
/// `thread` and `epoch` identify the region tick the event belongs to: the
/// epoch is the region's `published_epoch` counter (the same id the links
/// stamp), so `(thread, epoch)` names a tick unambiguously. `node_tick_end`
/// carries the value the node produced; a node whose `tick_fn` throws emits
/// `node_tick_throw` instead and the tick's `region_step_end` reports
/// `threw = true` (the exception still propagates).
///
/// `duration_ns` is a measured evaluation time in nanoseconds: the node's
/// `tick_fn` call on `node_tick_end` / `node_tick_throw` (never the sink
/// overhead), or the whole region tick on `region_step_end`.
///
/// The struct is trivially copyable so it can travel through a lock-free
/// queue from a ticking thread to a consumer.
struct trace_event
{
    trace_event_kind kind;
    kernel::clock_tag clock;
    conc::epoch_id epoch;
    kernel::node_id node_id;
    std::uint64_t sequence;
    kernel::value payload;
    std::uint64_t duration_ns;
};

/// Consumer of a `debug_engine`'s trace. Implementations must be cheap and
/// non-blocking enough for the emitting thread; a lock-free recorder is the
/// expected shape (see `tools/web-debugger`).
class trace_sink
{
public:
    virtual ~trace_sink() = default;

    /// Receive one trace event on the emitting thread.
    virtual void
    on_event(const trace_event&) = 0;
};

/// Append each event as one human-readable line to `target`.
class text_trace_sink final : public trace_sink
{
public:
    /// Build a sink appending to `target`; `target` must outlive the sink.
    explicit text_trace_sink(std::string& target);

    void
    on_event(const trace_event&) override;

private:
    std::string& m_target;
};

/// Print each event as one human-readable line to `std::cout`.
class cout_trace_sink final : public trace_sink
{
public:
    void
    on_event(const trace_event&) override;

private:
};

/// True when a trace payload is not the empty (`monostate`) marker: a node
/// evaluation's value is worth printing — a scalar literal, or a struct's
/// schema-driven text.
bool
is_present(const kernel::value&) noexcept;

/// Render a traced value as text, using the struct's schema when it has one.
std::string
value_text(const kernel::value&);

/// The trace spelling of a clock tag.
std::string
clock_text(kernel::clock_tag);

/// Render one event as a single line (the format `text_trace_sink` writes).
std::string
event_to_line(const trace_event&);

} // namespace clockr::kernel
