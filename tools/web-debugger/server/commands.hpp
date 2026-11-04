#pragma once

// The write-back command path: the bounded, per-(example, region) queues a
// debug client's SetInput/ReadInput/ReadOutput messages travel through, plus
// the free-function pair a host's tick loops call around engine.step.
//
// Why this exists. `program::set`/`get` are
// same-thread-with-the-owning-region contracts: they mutate or read the
// region's input/output storage, which that region's ticking thread is using
// during a tick. The server's uWebSockets loop thread therefore never touches
// a program. It only enqueues a *command*; the region's own tick thread
// applies it at a tick boundary:
//
//     agent ──ClientMessage──▶ server (uWS loop thread)
//                                  │  enqueue into an SPSC command queue
//     host ticker ────────────────▶│  (one queue per region)
//        engine.step(program, clock)  ← the loop, unchanged
//          └─ command_hook::before_step: program.set / get_input / get
//          └─ command_hook::after_step:  Ack with the consumed epoch
//     agent ◀──ServerMessage(Ack)─┘  (targeted at the requesting connection)
//
// The normal host path is attach-once: build a `command_hook` from this
// example's queues, hand it to `engine::set_step_hook`, and leave every
// tick loop as plain `engine.step(program, clock)`. That keeps the engine
// choice (debug_engine vs release_engine, debugger built in or out) a
// construction-time decision instead of an edit at each call site. The
// `command_drain` / `command_finish` free functions below are the primitives
// that hook is made of; advanced hosts and tests may call them directly.
//
// Applying at a boundary is not a workaround, it is the correct atomicity: in
// a clocked-synchronous FRP program a value set between ticks is
// indistinguishable from one set before the tick, and setting *during* a tick
// would violate the same-step laws (L1/L2 in
// doc/reference/denotational-semantics.md). Reads take the same boundary path
// for the same ownership reason — even reading is a host tick-loop act.
//
// Threading, per queue (the split exists because SPSC is the whole point):
//
//     command queue: producer = server loop thread (single),
//                    consumer = the owning region's tick thread (single);
//     reply queue:   producer = the owning region's tick thread (single),
//                    consumer = server loop thread (single).
//
// hence one command queue and one reply queue per (example, clock_tag), never
// one per example. Only `conc::spsc` is used; no new lock-free structure.
//
// The command direction carries plain data only: a `command_value` (the scalar
// projection of the wire Value), never a `kernel::value` holding a user cell.
// The loop thread converts the wire payload down at enqueue time (where an
// unsupported kind is an immediate error Ack), and the applying thread
// converts it back up into a kernel value. Read replies travel the other way
// as a `kernel::value`: the tick thread retains a reference for the crossing
// and the loop thread releases it after encoding — the same reference
// hand-back the trace recorder uses for traced structs.

#include "message.hpp"

#include "clockr/conc/link.hpp"
#include "clockr/conc/spsc.hpp"
#include "clockr/kernel/clock.hpp"
#include "clockr/kernel/engine.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/value.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace clockr::debug
{

// --- the wire value a SetInput may carry ---------------------------------

/// A scalar kernel value as it travels the command queue: the subset of the
/// wire `Value` a SetInput may carry in the debug server. `unit` is the monostate
/// payload, which a `set` uses to disengage an event input.
///
/// Structs and text are deliberately absent. The kernel's owning path for a
/// user struct is the typed `program::set<T>(name, args...)`, and there is no
/// runtime constructor from a `user_type` schema; injecting a struct needs a
/// host-provided materialization table, which is a later chunk's concern. The
/// loop thread rejects those payloads at enqueue time, so the tick thread only
/// ever sees this closed scalar set.
struct command_value
{
    enum class kind : std::uint8_t
    {
        unit,
        boolean,
        integer,
        real,
    };

    kind which { kind::unit };
    bool boolean { false };
    std::int64_t integer { 0 };
    double real { 0.0 };

    /// Project onto the kernel value set. Called on the applying thread, never
    /// on the producer side.
    kernel::value
    to_value() const noexcept;
};

/// Decode a SetInput's wire value. Returns nullopt and points `error` at a
/// static message when the payload is not injectable in the debug server; the caller
/// turns that into an immediate error Ack (no queue entry). An absent oneof
/// reads as monostate, the same convention the trace codec uses.
std::optional<command_value>
command_value_from_message(
    const pb::Value& message,
    std::string_view& error
) noexcept;

// --- queued commands and replies -----------------------------------------

/// What a queued command asks the host to do. Reads ride the same queue as
/// writes: `program` storage is ticking-thread-owned, so the read has to be a
/// boundary act too.
enum class command_kind : std::uint8_t
{
    set_input,
    read_input,
    read_output,
};

/// One command as it crosses the queue: opaque connection token (the server
/// routes the reply back to exactly that connection), the client's request id,
/// what to do, and the target port's positional index in the program's global
/// port order (what `program::set(index, ...)` / `get_input(index)` / `get`
/// take). The index is resolved from the port name on the loop thread, so the
/// tick thread never does a name lookup.
struct command
{
    std::uint64_t connection { 0 };
    std::uint64_t request_id { 0 };
    command_kind kind { command_kind::set_input };
    std::size_t port { 0 };
    command_value value {};
};

/// One reply as it crosses the queue back to the server loop thread. A
/// SetInput becomes an ack (`applied_epoch` set, `error` normally null); a read
/// becomes a port value (its snapshot `value` owns one reference while it is
/// queued — the loop thread releases it after encoding). `error` is a static
/// literal, so no allocation is needed on the tick thread; enqueue-time
/// validation errors are answered on the loop thread and never enter this
/// queue.
struct reply
{
    std::uint64_t connection { 0 };
    std::uint64_t request_id { 0 };
    command_kind kind { command_kind::set_input };
    std::size_t port { 0 };
    std::uint64_t applied_epoch { 0 };
    kernel::value value { std::monostate {} };
    const char* error { nullptr };
};

// --- the port directory --------------------------------------------------

/// One boundary port as the command path needs it: the name a client
/// addresses, the region that owns it, and its positional index in the
/// program's global port order. Built once per example from the program
/// description, so enqueue-time validation is a name lookup on the loop thread
/// and a bad name never costs the tick thread anything (AS-A3).
struct port_endpoint
{
    std::string name;
    kernel::clock_tag clock { kernel::clock_tag::audio };
    std::size_t index { 0 };
};

/// The command-path view of an example's boundary ports. Nameless ports (a
/// hand-written nameless descriptor) are skipped: a client can only address
/// ports by name.
struct port_directory
{
    std::vector<port_endpoint> inputs;
    std::vector<port_endpoint> outputs;

    /// Build from the program's port directory (declaration order, which is
    /// the order `program::set(index, ...)` uses).
    static port_directory
    build(const kernel::program&);

    const port_endpoint*
    find_input(std::string_view name) const noexcept;

    const port_endpoint*
    find_output(std::string_view name) const noexcept;

    /// Look a port up by its global positional index (for encoding a read
    /// reply, whose command carries the index it was validated with).
    const port_endpoint*
    find_input_index(std::size_t index) const noexcept;

    const port_endpoint*
    find_output_index(std::size_t index) const noexcept;
};

// --- the per-example queues ----------------------------------------------

/// The bounded, lock-free command and reply queues for one example: one of
/// each per clock tag (see this file's banner for why the split is per
/// region).
///
/// Ownership follows the recorder contract: the server owns this object and
/// hands the host's tick threads a stable reference through
/// `server::commands_for`. It lives until `remove_example(name)` returns; the
/// host must not touch it afterwards (remove_example discards anything still
/// queued, releasing the references read replies own). A dropped command is
/// counted and surfaced as `CommandStats`; it is never acked.
///
/// The two directions are deliberately `capacity`-sized each, but they are
/// *not* paced against each other: the tick thread can run many boundaries
/// between two reply drains of the uWS loop. That is why `command_drain`
/// rate-limits itself against the reply queue's actual free space (an exact
/// reserved-slot counter, not an approximate size probe): a command is only
/// consumed when its eventual reply fits, and the rest wait in the command
/// queue for a later boundary. One Ack per accepted command is therefore a
/// guarantee, not a race; a burst is answered late, never silently lost.
///
/// The drain/finish hand-off state (pending acks, the epoch snapshot) is
/// tick-thread-only and guarded by debug asserts; `command_drain` and
/// `command_finish` (or the `command_hook` built on them) are its only
/// users.
class example_commands
{
public:
    /// `capacity` bounds each region's command queue and its reply queue. The
    /// drain additionally gates on free reply slots (see the class note), so
    /// the two capacities do not have to match the producer/consumer pacing.
    /// The drain-and-finish scratch storage is reserved up front, so applying
    /// commands allocates nothing.
    explicit example_commands(std::size_t capacity);

    ~example_commands();

    example_commands(const example_commands&) = delete;
    example_commands&
    operator=(const example_commands&) = delete;

    // --- producer side (server loop thread) --------------------------------

    /// Enqueue a command for `clock`; false when that region's queue is full.
    /// The caller counts nothing extra — the drop is counted here (once per
    /// rejected command) and surfaced as CommandStats, and the command is never
    /// acked (AS-A4).
    bool
    enqueue(kernel::clock_tag, const command&);

    /// Pop one reply from any region, round-robin so a busy region cannot
    /// starve the others (in-region order is preserved exactly). False when
    /// every reply queue is empty.
    bool
    take_reply(reply&);

    /// Cumulative command-path losses since construction: commands that could
    /// not be enqueued, plus (defensively) replies that could not be queued —
    /// the latter is impossible under the capacity invariant above, but a lost
    /// reply must be visibly lost too, so it is counted rather than swallowed.
    std::uint64_t
    dropped() const noexcept;

    /// Discard everything still queued: commands a host will never apply (the
    /// example is going away) and replies the loop will never ship (releasing
    /// the references their values own). Loop thread only, after no tick
    /// thread can touch this example's queues any more — the same discipline
    /// `remove_example` applies to the recorder.
    void
    discard_pending();

    // --- consumer side (the owning region's tick thread) -------------------

    /// Pop one command for this region. Mainly for tests and hosts that drive
    /// their own loop; production hosts use `command_drain`.
    bool
    receive(kernel::clock_tag, command&);

    /// Post a reply. False only when the reply queue is full — impossible in
    /// the drain/finish protocol (see the capacity note above); `command_drain`
    /// counts it and releases the value.
    bool
    post(kernel::clock_tag, reply&&);

private:
    struct region_state
    {
        explicit region_state(std::size_t capacity) :
            commands { capacity },
            replies { capacity }
        {
            pending_acks.reserve(capacity);
        }

        conc::spsc<command> commands;
        conc::spsc<reply> replies;
        std::atomic<std::uint64_t> dropped { 0 };

        // Replies posted (or reserved by a consumed command) but not yet
        // shipped by the loop thread. Single producer (the region's tick
        // thread) increments it before enqueueing a reply; the single consumer
        // (the uWS loop thread) decrements it after popping one.
        // `command_drain` reads it to reserve reply slots before consuming
        // commands, so the reply queue is never overrun however far the ticker
        // runs ahead of the loop's drain timer. Relaxed is enough: the producer
        // only needs its own increments to be visible (so the count can only
        // read high, never low), and the consumer's decrements only ever free
        // slots.
        std::atomic<std::size_t> replies_outstanding { 0 };

        // Drain/finish hand-off, tick thread only: the SetInput commands whose
        // Acks still owe the epoch of the step being bracketed, and the
        // published epoch as it stood when the boundary began.
        std::vector<command> pending_acks;
        conc::epoch_id drain_epoch { 0 };
#ifndef NDEBUG
        bool draining { false };
        std::thread::id drain_thread {};
#endif
    };

    template<std::size_t... I>
    static std::array<region_state, kernel::clock_tag_count>
    make_regions(std::size_t capacity, std::index_sequence<I...>)
    {
        return { ((void)I, region_state { capacity })... };
    }

    static std::size_t
    slot(kernel::clock_tag tag) noexcept
    {
        return kernel::clock_tag_index(tag);
    }

    // The effective per-region capacity (0 is clamped: the underlying
    // reader-writer queue expects at least one slot).
    std::size_t m_capacity;

    std::array<region_state, kernel::clock_tag_count> m_regions;

    friend std::size_t
    command_drain(kernel::program&, kernel::clock_tag, example_commands&);

    friend void
    command_finish(
        kernel::program&,
        kernel::clock_tag,
        example_commands&,
        conc::epoch_id applied_epoch
    );
};

// --- the host contract, as two free functions ----------------------------

/// Apply every command queued for `clock`, on that region's tick thread,
/// immediately before `engine.step(program, clock)`:
///
///   * SetInput is applied with `program.set(index, value)`, so the queued
///     state precedes the step in program order;
///   * ReadInput / ReadOutput become PortValue replies from the current
///     snapshot (a read at the boundary the step is about to consume);
///   * a command whose apply throws is answered with an error reply instead of
///     killing the tick loop (validation makes this practically unreachable).
///
/// Acks are posted by `command_finish`, after the step, because they name the
/// epoch the step consumed. Consumes at most the number of commands the reply
/// queue has free slots for; the rest stay queued for a later boundary (see
/// the reply-backpressure note inside). Returns the number of commands
/// consumed.
///
/// Debug-build obligations (asserted): call it once per boundary, on one
/// thread, followed by `command_finish` before the next drain — the pair
/// brackets exactly one step.
std::size_t
command_drain(kernel::program&, kernel::clock_tag, example_commands&);

/// Post one Ack per command `command_drain` applied, stamped with
/// `applied_epoch`: the epoch of the tick that consumed them. A clean step
/// publishes that epoch; a throwing step does not, so the caller passes the
/// attempted epoch (drain epoch + 1) — the same epoch the trace's
/// `region_step_end` reports with `threw = true`. `command_hook` reads it
/// straight from `engine::step`'s `after_step` hook, which is why the caller
/// supplies it explicitly rather than re-reading `published_epoch` (wrong on
/// the throwing path).
///
/// Call it right after `engine.step`, on the same thread, and also from the
/// step's catch path: the command *was* consumed, so it must be acked.
void
command_finish(
    kernel::program&,
    kernel::clock_tag,
    example_commands&,
    conc::epoch_id applied_epoch
);

/// The normal host path: a `kernel::step_hook` that turns `engine::step`
/// into the debugger's tick-boundary command pump.
///
/// Build one per example over that example's `example_commands` (fetched with
/// `server::commands_for`) and attach it to every engine that ticks the
/// example — audio, ui, worker. The host's tick loops then stay exactly
/// `engine.step(program, clock)`; switching an engine from `debug_engine` to
/// `release_engine`, or building the debugger in or out, never touches them.
///
/// Threading: the hooks run on whichever thread ticks that region (the engine
/// guarantees one), and the adapter itself is stateless apart from the queues,
/// whose per-region SPSC discipline does the rest — the same `command_hook`
/// instance may serve several engines/regions concurrently.
class command_hook final : public kernel::step_hook
{
public:
    explicit command_hook(example_commands& commands) noexcept :
        m_commands { commands }
    {}

    /// Apply queued commands at the boundary, before the tick consumes state.
    void
    before_step(kernel::program& program, kernel::clock_tag clock) override;

    /// Answer them with the epoch of the tick that consumed them (the
    /// engine's attempted epoch on a throwing tick).
    void
    after_step(
        kernel::program& program,
        kernel::clock_tag clock,
        conc::epoch_id epoch,
        bool threw
    ) override;

private:
    example_commands& m_commands;
};

// ############### IMPLEMENTATION ################

inline kernel::value
command_value::to_value() const noexcept
{
    switch (which)
    {
        case kind::unit:
            return kernel::value { std::monostate {} };
        case kind::boolean:
            return kernel::value { boolean };
        case kind::integer:
            return kernel::value { integer };
        case kind::real:
            return kernel::value { real };
        default:
            // Unreachable: the closed kind set is fully listed above.
            return kernel::value { std::monostate {} };
    }
}

inline std::optional<command_value>
command_value_from_message(
    const pb::Value& message,
    std::string_view& error
) noexcept
{
    command_value value;

    switch (message.kind_case())
    {
        case pb::Value::kUnit:
            value.which = command_value::kind::unit;
            return value;
        case pb::Value::kBoolValue:
            value.which = command_value::kind::boolean;
            value.boolean = message.bool_value();
            return value;
        case pb::Value::kIntValue:
            value.which = command_value::kind::integer;
            value.integer = static_cast<std::int64_t>(message.int_value());
            return value;
        case pb::Value::kDoubleValue:
            value.which = command_value::kind::real;
            value.real = message.double_value();
            return value;
        case pb::Value::kStructValue:
            // Deferred: a struct needs a runtime constructor from its schema,
            // which only a host can provide (the kernel's owning path is the
            // typed program::set<T>). Chunk B/C may add a materialization
            // table; until then the error is explicit, not silent.
            error = "unsupported value kind: user structs need a typed setter "
                    "(deferred)";
            return std::nullopt;
        case pb::Value::kStringValue:
            error = "unsupported value kind: text has no kernel input "
                    "representation";
            return std::nullopt;
        case pb::Value::KIND_NOT_SET:
            // Absent payload == monostate, matching from_message(pb::Value)
            // and the trace codec's "unset oneof reads back as monostate".
            value.which = command_value::kind::unit;
            return value;
        default:
            error = "unsupported value kind";
            return std::nullopt;
    }
}

inline port_directory
port_directory::build(const kernel::program& program)
{
    port_directory directory;

    const auto graph { program.descriptor() };

    directory.inputs.reserve(graph.input_ids.size());

    for (std::size_t i { 0 }; i < graph.input_ids.size(); ++i)
    {
        const auto* name { graph.input_names[i] };

        if (name == nullptr || *name == '\0')
            continue; // nameless descriptor: not addressable over the wire

        directory.inputs.push_back(
            port_endpoint {
                name, kernel::clock(at(graph, graph.input_ids[i])), i }
        );
    }

    directory.outputs.reserve(graph.output_ids.size());

    for (std::size_t i { 0 }; i < graph.output_ids.size(); ++i)
    {
        const auto* name { graph.output_names[i] };

        if (name == nullptr || *name == '\0')
            continue;

        directory.outputs.push_back(
            port_endpoint {
                name, kernel::clock(at(graph, graph.output_ids[i])), i }
        );
    }

    return directory;
}

inline const port_endpoint*
port_directory::find_input(std::string_view name) const noexcept
{
    for (const auto& port : inputs)
        if (std::string_view { port.name } == name)
            return &port;

    return nullptr;
}

inline const port_endpoint*
port_directory::find_output(std::string_view name) const noexcept
{
    for (const auto& port : outputs)
        if (std::string_view { port.name } == name)
            return &port;

    return nullptr;
}

inline const port_endpoint*
port_directory::find_input_index(std::size_t index) const noexcept
{
    for (const auto& port : inputs)
        if (port.index == index)
            return &port;

    return nullptr;
}

inline const port_endpoint*
port_directory::find_output_index(std::size_t index) const noexcept
{
    for (const auto& port : outputs)
        if (port.index == index)
            return &port;

    return nullptr;
}

inline example_commands::example_commands(std::size_t capacity) :
    m_capacity { capacity == 0 ? std::size_t { 1 } : capacity },
    m_regions { make_regions(
        m_capacity,
        std::make_index_sequence<kernel::clock_tag_count> {}
    ) }
{}

inline example_commands::~example_commands()
{
    // Replies the loop never shipped still own a reference each (see reply);
    // hand them back, exactly like trace_recorder's destructor. Runs on the
    // owner's thread, with the program and its homes still alive.
    discard_pending();
}

inline bool
example_commands::enqueue(kernel::clock_tag tag, const command& cmd)
{
    auto& region { m_regions[slot(tag)] };

    if (region.commands.send(cmd))
        return true;

    // AS-A4: a lost command is visibly lost. Count it here, once, and let the
    // caller stay silent — there is deliberately no Ack for a dropped command.
    region.dropped.fetch_add(1, std::memory_order_relaxed);

    return false;
}

inline bool
example_commands::take_reply(reply& out)
{
    for (auto& region : m_regions)
    {
        if (!region.replies.receive(out))
            continue;

        // Ship-side half of the reserve/serve protocol: freeing the slot is
        // what lets the next command_drain consume another command.
        region.replies_outstanding.fetch_sub(1, std::memory_order_relaxed);

        return true;
    }

    return false;
}

inline std::uint64_t
example_commands::dropped() const noexcept
{
    std::uint64_t total { 0 };

    for (const auto& region : m_regions)
        total += region.dropped.load(std::memory_order_relaxed);

    return total;
}

inline void
example_commands::discard_pending()
{
    for (auto& region : m_regions)
    {
        command discarded_command;

        while (region.commands.receive(discarded_command))
        {
        }

        reply discarded_reply;

        while (region.replies.receive(discarded_reply))
            kernel::release(discarded_reply.value);

        // Nothing can be in flight once the queue is empty and no tick thread
        // may touch the region: reset the reserved-slot count exactly.
        region.replies_outstanding.store(0, std::memory_order_relaxed);
        region.pending_acks.clear();

#ifndef NDEBUG
        region.draining = false;
        region.drain_thread = std::thread::id {};
#endif
    }
}

inline bool
example_commands::receive(kernel::clock_tag tag, command& out)
{
    return m_regions[slot(tag)].commands.receive(out);
}

inline bool
example_commands::post(kernel::clock_tag tag, reply&& out)
{
    auto& region { m_regions[slot(tag)] };

    // Reserve the slot *before* enqueueing: a single consumer may pop the
    // reply immediately, and its decrement must never run before the matching
    // increment (that would underflow the counter). `command_drain` reserves
    // one slot per command it consumes, reads and set-acks alike, so this only
    // fails if a host bypassed the drain/finish contract — a host bug, not
    // something a remote client can drive. Count the loss and carry on: a
    // debugging tool must not abort its host over a reply it cannot queue.
    region.replies_outstanding.fetch_add(1, std::memory_order_relaxed);

    if (region.replies.send(std::move(out)))
        return true;

    region.replies_outstanding.fetch_sub(1, std::memory_order_relaxed);
    region.dropped.fetch_add(1, std::memory_order_relaxed);

    return false;
}

inline std::size_t
command_drain(
    kernel::program& program,
    kernel::clock_tag clock,
    example_commands& commands
)
{
    auto& region { commands.m_regions[example_commands::slot(clock)] };

#ifndef NDEBUG
    assert(
        !region.draining && "command_drain called twice without command_finish"
    );
    region.draining = true;
    region.drain_thread = std::this_thread::get_id();
#endif

    // The epoch this boundary is about to build: command_finish names it in
    // the Ack. For a clean step it equals the published_epoch after
    // engine.step; a throwing tick never publishes, so keeping the snapshot
    // lets finish name the attempted tick instead.
    region.drain_epoch =
        program.region(clock).published_epoch.load(std::memory_order_acquire);

    // Reply backpressure (exact, not approximate): consume only as many
    // commands as the reply queue has free slots. Every consumed command
    // reserves one — reads post their PortValue during this call, SetInput
    // acks post in command_finish. Without the gate a hot region (the demo's
    // audio thread ticks every few ms while the uWS loop ships replies every
    // 33ms) runs many boundaries between two reply drains and out-runs the
    // bounded reply queue; the old "replies can never exceed accepted
    // commands" argument ignored that pacing mismatch. The rest stay queued
    // and are answered at a later boundary, so every accepted command is still
    // acked exactly once (a burst is answered late, never silently lost).
    const auto reserved {
        region.replies_outstanding.load(std::memory_order_acquire) +
        region.pending_acks.size()
    };
    const auto budget { reserved >= commands.m_capacity
                            ? std::size_t { 0 }
                            : commands.m_capacity - reserved };

    std::size_t consumed { 0 };
    command entry;

    while (consumed < budget && region.commands.receive(entry))
    {
        // Every consumed command reserves one reply slot, whatever its outcome
        // (an apply failure still posts an error reply).
        ++consumed;

        const auto reply_for { [&entry](command_kind kind) {
            reply out;
            out.connection = entry.connection;
            out.request_id = entry.request_id;
            out.kind = kind;
            out.port = entry.port;

            return out;
        } };

        switch (entry.kind)
        {
            case command_kind::set_input:
            {
                try
                {
                    program.set(entry.port, entry.value.to_value());
                    region.pending_acks.push_back(entry);
                }
                catch (const std::exception&)
                {
                    // Validation on the loop thread makes a failing apply
                    // practically unreachable; answer an error rather than
                    // let an exception escape a tick loop.
                    auto out { reply_for(command_kind::set_input) };
                    out.error = "command rejected at the tick boundary";
                    commands.post(clock, std::move(out));
                }
                break;
            }
            case command_kind::read_input:
            {
                auto out { reply_for(command_kind::read_input) };

                try
                {
                    out.value = program.get_input(entry.port);
                    kernel::retain(
                        out.value
                    ); // the reply owns a reference across the threads
                }
                catch (const std::exception&)
                {
                    out.value = kernel::value { std::monostate {} };
                    out.error = "command rejected at the tick boundary";
                }

                const kernel::value snapshot { out.value };

                if (!commands.post(clock, std::move(out)))
                    kernel::release(snapshot);

                break;
            }
            case command_kind::read_output:
            {
                auto out { reply_for(command_kind::read_output) };

                try
                {
                    out.value = program.get(entry.port);
                    kernel::retain(out.value);
                }
                catch (const std::exception&)
                {
                    out.value = kernel::value { std::monostate {} };
                    out.error = "command rejected at the tick boundary";
                }

                const kernel::value snapshot { out.value };

                if (!commands.post(clock, std::move(out)))
                    kernel::release(snapshot);

                break;
            }
            default:
                break;
        }
    }

    return consumed;
}

inline void
command_finish(
    kernel::program& program,
    kernel::clock_tag clock,
    example_commands& commands,
    conc::epoch_id applied_epoch
)
{
    auto& region { commands.m_regions[example_commands::slot(clock)] };

    // `program` is otherwise unused here (the epoch comes from the caller: the
    // engine knows it on both paths, while `published_epoch` is stale after a
    // throwing tick); keep the parameter for the host-facing primitive shape
    // and to let debug builds cross-check the epoch we were handed.
    (void)program;

#ifndef NDEBUG
    assert(
        region.draining && "command_finish without a matching command_drain"
    );
    assert(
        region.drain_thread == std::this_thread::get_id() &&
        "command_finish must run on the region's tick thread, like the step it "
        "brackets"
    );
    assert(
        applied_epoch == region.drain_epoch + 1 &&
        "command_finish must be stamped with the epoch of the tick that "
        "consumed the commands (a clean step publishes it; a throwing step "
        "carries the attempted epoch)"
    );
#endif

    for (const auto& ack : region.pending_acks)
    {
        reply out;
        out.connection = ack.connection;
        out.request_id = ack.request_id;
        out.kind = command_kind::set_input;
        out.port = ack.port;
        out.applied_epoch = applied_epoch;

        commands.post(clock, std::move(out));
    }

    region.pending_acks.clear();

#ifndef NDEBUG
    region.draining = false;
    region.drain_thread = std::thread::id {};
#endif
}

inline void
command_hook::before_step(kernel::program& program, kernel::clock_tag clock)
{
    command_drain(program, clock, m_commands);
}

inline void
command_hook::after_step(
    kernel::program& program,
    kernel::clock_tag clock,
    conc::epoch_id epoch,
    bool threw
)
{
    // A throwing tick publishes no epoch; the engine hands us the attempted
    // one, so the Ack still names the tick the command was consumed by — the
    // same epoch the trace reports with threw=true.
    (void)threw;

    command_finish(program, clock, m_commands, epoch);
}

} // namespace clockr::debug
