#pragma once

#include "commands.hpp"
#include "message.hpp"
#include "recorder.hpp"

#include "clockr/kernel/program.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations only (the uWebSockets App and the libusockets loop and
// timer the server runs on); the implementation includes the full headers.
struct us_loop_t;
struct us_timer_t;

namespace uWS
{
template <bool>
class TemplatedApp;
} // namespace uWS

namespace clockr::debug
{

struct server_options
{
    /// Interface to bind. The default is loopback: a debugger has no business
    /// on the LAN, and embedded hosts (a plugin serving its live program) must
    /// not expose a port beyond the machine. A deployment that fronts the
    /// server with a restricted proxy can opt into a wider bind explicitly.
    std::string host { "127.0.0.1" };
    std::uint16_t port { 9002 };
    std::string web_root { "tools/web-debugger/web" };

    /// Drain period: events leave the producer queues and reach clients this
    /// often. 30 Hz is plenty for a human-facing timeline and keeps a slow
    /// client from applying backpressure to the producer.
    std::uint64_t drain_period_ms { 33 };

    /// History kept for clients that connect late (and for reconnects): the
    /// last N events per thread tag are replayed on connect.
    std::size_t history_per_thread { 100'000 };

    /// Upper bound on events moved per drain; the remainder stays queued.
    std::size_t drain_batch { 16'384 };

    /// Per region, how many write-back commands the server may queue for the
    /// host's tick thread (the reply queue is the same size). Bounded on
    /// purpose: a client that floods the debugger must cost the host a bounded
    /// amount of memory and, when the command queue itself is full, a counted
    /// drop (surfaced as CommandStats) — never unbounded growth. This is *not*
    /// a reply bound: the tick thread may run far ahead of the loop's reply
    /// drains, so `command_drain` gates on free reply slots and leaves the
    /// overflow queued for later boundaries. Every accepted command is
    /// answered exactly once; only enqueue-time overflow loses commands.
    std::size_t command_queue_capacity { 1024 };
};

/// One command-path reply on its way to exactly one client connection. Debug
/// clients' request ids are only meaningful to the connection that sent them,
/// so replies are never broadcast (unlike the observation stream).
struct outbound_frame
{
    std::uint64_t connection { 0 };
    std::string frame;
};

/// One live example: a program to tick plus the recorder its engines report
/// to. Each example quarantines its own trace — a ticking thread that is
/// mid-step across a switch lands its events in the old example's recorder,
/// which is never drained while another example is active, so timelines
/// cannot mix programs. Only the active example's recorder captures (see
/// server::sync_recorder_capture): a dormant recorder drains nothing, so
/// enabled capture on it would strand one pool-cell reference per traced
/// payload until the example's pools exhaust. The recorders (and programs)
/// are owned by the host; they must outlive the server.
struct example
{
    std::string name;
    const kernel::program& program;
    trace_recorder& recorder;
};

/// The WebSocket server: serves the static UI over HTTP, streams a
/// GraphDescription once per connection, and EventBatches on a fixed cadence.
///
/// The server ticks nothing itself; the host steps whichever example is
/// active (see active()). Switching examples is a uWS-loop-thread act from
/// end to end — flip the index, quarantine both recorders' backlogs, clear
/// the history — so no lock is needed anywhere here. Ticking threads only
/// ever read the index, once per loop iteration, and step that program fully.
///
/// All of the server's own state is touched from uWebSockets' single event
/// loop (the drain timer, client send/receive, history mutation), so history
/// needs no lock. The producer queues are the only cross-thread hand-off and
/// they are lock-free.
///
/// Multi-instance: every server object is independent (the drain timer carries
/// its own server pointer; no process globals). Two servers may coexist, but
/// only one can bind a given port — the loser prints a listen error and its
/// run() returns, so an embedded host whose port is taken simply serves
/// nothing. See stop() for shutdown.
///
/// Dynamic examples: the example set may be empty at construction and grow or
/// shrink at runtime (add_example / remove_example). These are the only
/// methods safe to call from a thread other than the uWS loop; a mutex
/// serializes them against the loop's drain/snapshot, so remove_example
/// returning guarantees no further access to that example's recorder. The
/// loop notices the change at its next drain tick and broadcasts an
/// ExampleList (and, if the live example moved, the new graph).
class server
{
public:
    /// The examples may be empty; the first is active at startup.
    server(std::vector<example> examples, server_options options);

    /// Blocks until the loop stops (stop(), SIGINT, or a listen failure).
    void
    run();

    /// Ask the loop to shut down: close every client connection, the drain
    /// timer and the listen socket, then return from run(). Safe to call from
    /// any thread, any number of times; the teardown itself runs on the loop
    /// thread at the next timer tick (at most one drain period late). Join the
    /// run() thread afterwards. A plugin host calls this on unload so no
    /// thread or socket outlives the module.
    void
    stop();

    /// Index of the example ticking threads should step. Read once per loop
    /// iteration and use consistently — a set on one program followed by a
    /// step on another is a host bug the type system cannot catch.
    std::size_t
    active() const noexcept;

    /// Registry lookup, for startup flags and tests.
    std::optional<std::size_t>
    index_of(std::string_view name) const;

    /// Switch the live example. Unknown names and re-selecting the active one
    /// are no-ops (false); otherwise flips the index, discards every
    /// recorder's backlog (releasing struct references), zeroes drop counters
    /// and clears the history, and returns true. The caller broadcasts the
    /// new snapshot afterwards — clients reset their whole view on any graph
    /// message. uWS-loop-thread only.
    bool
    select_example(std::string_view name);

    // --- dynamic examples (any thread) -------------------------------------

    /// Register a new example. The first example added to an empty server
    /// becomes active. `example.program`/`example.recorder` must outlive the
    /// example: call remove_example(name) (and let it return) before
    /// destroying them. The loop broadcasts the new list at its next tick.
    void
    add_example(example entry);

    /// Unregister the example named `name`. Synchronous with respect to the
    /// loop: once this returns, the server holds no reference to that
    /// example's recorder, so the caller may destroy it. Removing the active
    /// example moves the active index to a neighbour. Returns false when no
    /// such example exists.
    bool
    remove_example(std::string_view name);

    /// The number of registered examples.
    std::size_t
    example_count() const;

    /// Serialized ExampleList of the current names and active example.
    std::string
    examples_snapshot() const;

    // --- event loop callbacks (called on the uWS thread) -------------------

    /// Serialized GraphDescription of the active example, for a new client —
    /// or for every client right after a switch.
    std::string
    snapshot() const;

    /// Move the active recorder's queued events into history and return one
    /// serialized ServerMessage per non-empty thread batch to broadcast (in
    /// order).
    std::vector<std::string>
    drain();

    // --- write-back command path -------------------------------------------

    /// The command queues for the example named `name`, for the host's tick
    /// loops (`command_drain` before each step, `command_finish` after it).
    /// Stable until `remove_example(name)` returns — the same lifetime contract
    /// as the recorder reference the example holds. Null when no such example
    /// exists. Any thread; the returned object's queues are the only
    /// cross-thread part. Mutable because draining is a queue operation, not a
    /// read: the host plays the consumer side of each SPSC pair.
    example_commands*
    commands_for(std::string_view name);

    /// Decode one client frame received on `connection` (the opaque token the
    /// server assigned that client) and act on it: `pause`/`resume`/
    /// `select_example` keep their observation semantics; `set_input` /
    /// `read_input` / `read_output` validate against the active example on the
    /// spot and enqueue a command, or queue an immediate error reply.
    /// uWS-loop-thread only; tests call it directly (the uWS onMessage handler
    /// is this call plus a token lookup). Invalid frames are ignored, as
    /// before.
    void
    handle_client_frame(std::uint64_t connection, std::string_view frame);

    /// Everything the command path owes clients right now: per-connection reply
    /// frames queued by tick threads (plus immediate validation errors), and a
    /// CommandStats broadcast frame when the cumulative drop count moved since
    /// the last broadcast. The timer routes each targeted frame to its
    /// connection and publishes `stats` on the "events" subscription.
    /// uWS-loop-thread only.
    struct command_outbox
    {
        std::vector<outbound_frame> replies;
        std::optional<std::string> stats;
    };

    command_outbox
    drain_commands();

private:
    // Drain-timer callback (a static member so it converts to the plain
    // function pointer uWS requires); forwards to the owning server.
    static void
    on_timer(us_timer_t* timer);

    // Drain-timer tick, on the loop thread: either broadcast one drain window
    // or, once stop() was requested, tear the loop down (clients, timer,
    // listen socket) so run() returns.
    void
    on_timer_tick(us_timer_t* timer);

    // Locked helpers (callers hold m_mutex). `index_of` and `select_example`
    // wrap these for the loop/test threads.
    std::optional<std::size_t>
    index_of_locked(std::string_view name) const;

    bool
    select_example_locked(std::string_view name);

    std::string
    active_name_locked() const;

    // Capture bookkeeping invariant (callers hold m_mutex; the constructor
    // runs before any thread exists and may also call directly). drain()
    // consumes exactly one recorder — the active example's — so an enabled
    // dormant recorder would strand one user-cell reference per traced
    // payload in a queue nothing drains, exhausting the dormant example's
    // own pools under its ticker (bad_alloc from the pool allocator, not
    // corruption). Re-impose "only the active example captures" here after
    // every change of active-ness or capture state: the active example's
    // recorder honours m_capture, every other one is disabled.
    void
    sync_recorder_capture() noexcept;

    // Validation and enqueue for one decoded write-back command (loop thread;
    // takes m_mutex). `value` is null for reads. `example_name` is the wire's
    // optional addressing override, null when absent.
    void
    handle_command(
        std::uint64_t connection,
        std::uint64_t request_id,
        command_kind,
        std::string_view port,
        const std::string* example_name,
        const pb::Value* value
    );

    // Queue one immediate error reply for `connection` (loop thread).
    void
    queue_error_reply(std::uint64_t connection, std::uint64_t request_id, std::string message);

    // Serialized CommandStats for the active example (loop thread). Used on
    // connect to tell a late client what it missed; resetting the broadcast
    // marker is the timer's business, not this helper's.
    std::string
    command_stats_frame() const;

    // Indirected because `example` holds references (so it is not assignable,
    // which vector::erase would need); the host still owns the referents.
    std::vector<std::unique_ptr<example>> m_examples;
    std::atomic<std::size_t> m_active { 0 };
    server_options m_options;

    // Serializes the dynamic-example mutators (add/remove, any thread) against
    // the loop's drain/snapshot/select. Held across a whole drain, so
    // remove_example returning proves the recorder is no longer in use.
    mutable std::mutex m_mutex;

    // Bumped under m_mutex whenever the example set (or active) changes; the
    // loop thread broadcasts an ExampleList when its cached value differs.
    std::atomic<std::uint64_t> m_revision { 0 };
    std::uint64_t m_broadcast_revision { 0 };
    std::string m_broadcast_active;

    // Capture gate (pause/resume), applied to examples registered later too.
    std::atomic<bool> m_capture { true };

    // The running App and its loop, set on the loop thread for the duration
    // of run(). stop() only flips m_stopping and wakes the loop; the teardown
    // itself runs on the loop thread (see on_timer in server.cpp), so closing
    // handles never races the loop.
    uWS::TemplatedApp<false>* m_app { nullptr };
    std::atomic<us_loop_t*> m_loop { nullptr };
    std::atomic<bool> m_running { false };
    std::atomic<bool> m_stopping { false };

    // One open client connection: the token replies are routed by, and the
    // opaque uWS socket handle. Loop-thread only (open adds, close removes,
    // the stop teardown closes the rest).
    struct connection
    {
        std::uint64_t token { 0 };
        void* socket { nullptr };
    };

    std::vector<connection> m_connections;

    // Token source for connections (loop-thread only). Tokens are opaque and
    // never reused, so a reply queued for a closed connection can never be
    // delivered to a later one.
    std::uint64_t m_next_connection { 0 };

    // Serialized once per example at construction; a session's networks are
    // static, only the active index moves.
    std::vector<std::string> m_graphs;

    // The command path, parallel to m_examples/m_graphs: the port directory a
    // command is validated against (descriptive data, loop-thread only) and
    // the per-(example, region) queues the host tick threads drain. Owned by
    // the server because the host only ever holds a stable reference
    // (commands_for); remove_example discards and destroys both under m_mutex,
    // so a tick thread cannot outlive its queues.
    std::vector<port_directory> m_ports;
    std::vector<std::unique_ptr<example_commands>> m_commands;

    // Replies queued on the loop thread itself (immediate validation errors),
    // waiting for the next drain_commands. Loop-thread only.
    std::vector<outbound_frame> m_reply_outbox;

    // Last CommandStats value broadcast, so the timer only publishes when the
    // cumulative count moves. Loop-thread only.
    std::uint64_t m_broadcast_dropped { 0 };

    std::vector<recorded_event> m_drain_buffer;

    // Per thread tag, the tail of the stream in send order, bounded. Holds
    // only the active example's events (cleared on every switch).
    std::array<std::deque<pb::TraceEvent>, kernel::clock_tag_count> m_history;
};

} // namespace clockr::debug
