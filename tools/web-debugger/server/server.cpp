#include "server.hpp"

#include <App.h>

#include <libusockets.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace clockr::debug
{

namespace
{
    // Per-timer context, carried in the timer's extension memory so every
    // server object is independent.
    struct timer_context
    {
        server* self { nullptr };
        us_listen_socket_t* listen_socket { nullptr };
    };

    std::string
    serialize(const pb::ServerMessage& message)
    {
        std::string bytes;
        bytes.resize(static_cast<std::size_t>(message.ByteSizeLong()));

        if (!message.SerializeToArray(bytes.data(), static_cast<int>(bytes.size())))
            bytes.clear();

        return bytes;
    }

    // One write-back acknowledgement. `error` is null for a clean apply (the
    // field stays off the wire); a command that never reached a boundary is
    // acked with applied_epoch 0, a command that did is acked with the epoch of
    // the tick that consumed it.
    std::string
    ack_frame(std::uint64_t request_id, const char* error, std::uint64_t applied_epoch)
    {
        pb::ServerMessage message;
        auto* ack { message.mutable_ack() };

        ack->set_request_id(request_id);

        if (error != nullptr)
            ack->set_error(error);

        ack->set_applied_epoch(applied_epoch);

        return serialize(message);
    }

    // One read answer: the snapshot the tick thread took, in the same Value
    // form the trace uses.
    std::string
    port_value_frame(
        std::uint64_t request_id,
        std::string_view port,
        const char* error,
        const kernel::value& value)
    {
        pb::ServerMessage message;
        auto* answer { message.mutable_port_value() };

        answer->set_request_id(request_id);
        answer->set_port(std::string { port });

        if (error != nullptr)
            answer->set_error(error);
        else
            *answer->mutable_value() = to_message(value);

        return serialize(message);
    }

    // Serialize one example's graph description (its program shape, named).
    std::string
    describe_example(const example& example)
    {
        pb::ServerMessage message;
        *message.mutable_graph() = describe(example.program);
        message.mutable_graph()->set_name(example.name);

        return serialize(message);
    }

    std::string
    content_type(std::string_view path)
    {
        if (path.ends_with(".html"))
            return "text/html";
        if (path.ends_with(".js"))
            return "text/javascript";
        if (path.ends_with(".css"))
            return "text/css";
        if (path.ends_with(".json"))
            return "application/json";
        if (path.ends_with(".wasm"))
            return "application/wasm";
        if (path.ends_with(".svg"))
            return "image/svg+xml";

        return "application/octet-stream";
    }

    std::string
    read_file(const std::string& path)
    {
        std::ifstream file { path, std::ios::binary };

        if (!file)
            return {};

        std::ostringstream contents;
        contents << file.rdbuf();

        return contents.str();
    }

    struct per_socket_data
    {
        std::uint64_t last_sequence { 0 };

        // The opaque per-connection token the server assigns on open: replies
        // to this client's write-back commands are routed by it (see
        // server::drain_commands).
        std::uint64_t token { 0 };
    };

    using socket_type = uWS::WebSocket<false, true, per_socket_data>;
} // namespace

void
server::on_timer(us_timer_t* timer)
{
    auto* context { static_cast<timer_context*>(us_timer_ext(timer)) };

    if (context == nullptr || context->self == nullptr)
        return;

    context->self->on_timer_tick(timer);
}

server::server(std::vector<example> examples, server_options options) :
    m_options { std::move(options) }
{
    m_examples.reserve(examples.size());
    m_ports.reserve(examples.size());
    m_commands.reserve(examples.size());

    for (auto& entry : examples)
    {
        m_graphs.push_back(describe_example(entry));
        m_ports.push_back(port_directory::build(entry.program));
        m_commands.push_back(std::make_unique<example_commands>(m_options.command_queue_capacity));
        m_examples.push_back(std::make_unique<example>(std::move(entry)));
    }

    m_drain_buffer.reserve(m_options.drain_batch);

    // The set is fixed at construction and the first example is active;
    // impose the single-recorder invariant before any thread can tick.
    sync_recorder_capture();
}

std::size_t
server::active() const noexcept
{
    return m_active.load(std::memory_order_acquire);
}

std::optional<std::size_t>
server::index_of(std::string_view name) const
{
    std::lock_guard<std::mutex> lock { m_mutex };
    return index_of_locked(name);
}

std::optional<std::size_t>
server::index_of_locked(std::string_view name) const
{
    for (std::size_t i { 0 }; i < m_examples.size(); ++i)
        if (m_examples[i]->name == name)
            return i;

    return std::nullopt;
}

std::string
server::active_name_locked() const
{
    const auto index { m_active.load(std::memory_order_acquire) };

    return index < m_examples.size() ? m_examples[index]->name : std::string {};
}

void
server::sync_recorder_capture() noexcept
{
    const bool capture { m_capture.load(std::memory_order_relaxed) };
    const std::size_t active { m_active.load(std::memory_order_acquire) };

    for (std::size_t i { 0 }; i < m_examples.size(); ++i)
        m_examples[i]->recorder.set_enabled(capture && i == active);
}

bool
server::select_example(std::string_view name)
{
    std::lock_guard<std::mutex> lock { m_mutex };
    return select_example_locked(name);
}

bool
server::select_example_locked(std::string_view name)
{
    const auto found { index_of_locked(name) };

    if (!found.has_value() || *found == m_active.load(std::memory_order_acquire))
        return false;

    m_active.store(*found, std::memory_order_release);

    // Quarantine: neither the newly-active backlog (stale ticks from before)
    // nor the old example's in-flight events (a thread mid-step across the
    // flip lands them here) may reach the new timeline. Drop counters reset
    // for the same reason — the new example starts un-dropped.
    for (auto& example : m_examples)
    {
        example->recorder.discard_pending();
        example->recorder.take_dropped();
    }

    for (auto& history : m_history)
        history.clear();

    // The flip moves live-ness: the old example's recorder stops capturing
    // (its backlog was just discarded, so its pools are already clean) and
    // the new one starts honouring the global capture switch.
    sync_recorder_capture();

    return true;
}

void
server::add_example(example entry)
{
    std::lock_guard<std::mutex> lock { m_mutex };

    m_graphs.push_back(describe_example(entry));
    m_ports.push_back(port_directory::build(entry.program));
    m_commands.push_back(std::make_unique<example_commands>(m_options.command_queue_capacity));
    m_examples.push_back(std::make_unique<example>(std::move(entry)));

    if (m_examples.size() == 1)
        m_active.store(0, std::memory_order_release);

    // A new example is never active unless it is the first; the invariant
    // (also pause/resume state) is re-imposed here rather than by copying
    // m_capture into the new recorder directly.
    sync_recorder_capture();

    m_revision.fetch_add(1, std::memory_order_release);
}

bool
server::remove_example(std::string_view name)
{
    std::lock_guard<std::mutex> lock { m_mutex };

    const auto found { index_of_locked(name) };

    if (!found.has_value())
        return false;

    const auto index { *found };

    // Drop anything still queued for the example before it disappears, releasing
    // the references pending read replies own (the host destroys the program
    // only after this returns). m_mutex is held, so no drain can race this and
    // no tick thread may legally touch the queues any more.
    m_commands[index]->discard_pending();

    m_examples.erase(m_examples.begin() + static_cast<std::ptrdiff_t>(index));
    m_graphs.erase(m_graphs.begin() + static_cast<std::ptrdiff_t>(index));
    m_ports.erase(m_ports.begin() + static_cast<std::ptrdiff_t>(index));
    m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(index));

    // Keep the active index pointing at the same example where possible; when
    // the active one itself goes, fall back to its neighbour (the element that
    // shifted into its place, or the new last one).
    auto active { m_active.load(std::memory_order_acquire) };

    if (m_examples.empty())
        active = 0;
    else if (index < active)
        --active;
    else if (index == active && active >= m_examples.size())
        active = m_examples.size() - 1;

    m_active.store(active, std::memory_order_release);

    // The neighbour that inherits live-ness inherits capture too.
    sync_recorder_capture();

    m_revision.fetch_add(1, std::memory_order_release);

    return true;
}

std::size_t
server::example_count() const
{
    std::lock_guard<std::mutex> lock { m_mutex };
    return m_examples.size();
}

std::string
server::examples_snapshot() const
{
    std::lock_guard<std::mutex> lock { m_mutex };

    pb::ServerMessage message;
    auto* list { message.mutable_examples() };

    for (const auto& example : m_examples)
        list->add_names(example->name);

    if (!m_examples.empty())
        list->set_active(active_name_locked());

    return serialize(message);
}

std::string
server::snapshot() const
{
    std::lock_guard<std::mutex> lock { m_mutex };

    const auto index { m_active.load(std::memory_order_acquire) };

    return index < m_graphs.size() ? m_graphs[index] : std::string {};
}

std::vector<std::string>
server::drain()
{
    std::lock_guard<std::mutex> lock { m_mutex };

    std::vector<std::string> out;

    if (m_examples.empty())
        return out;

    auto& recorder { m_examples[m_active.load(std::memory_order_acquire)]->recorder };

    m_drain_buffer.clear();
    recorder.drain(m_drain_buffer, m_options.drain_batch);

    // Every drained event owns one reference to its payload (see
    // trace_recorder::on_event), spent once the event has been encoded — and
    // on the throwing path too, so the buffer can never strand a reference.
    struct payload_release
    {
        std::vector<recorded_event>& events;

        ~payload_release()
        {
            for (const auto& recorded : events)
                kernel::release(recorded.event.payload);
        }
    } release_payloads { m_drain_buffer };

    const auto dropped { recorder.take_dropped() };

    // Group this drain's events by thread tag (the queues are per tag, but a
    // round-robin drain interleaves them; the UI wants per-thread batches).
    std::array<pb::EventBatch, kernel::clock_tag_count> batches;

    for (std::size_t i { 0 }; i < kernel::clock_tag_count; ++i)
        batches[i].set_clock(to_message(static_cast<kernel::clock_tag>(i)));

    for (const auto& recorded : m_drain_buffer)
    {
        const std::size_t slot { kernel::clock_tag_index(recorded.event.clock) };

        auto* event { batches[slot].add_events() };
        *event = to_message(recorded.event, recorded.stamp_ns);

        auto& history { m_history[slot] };
        history.push_back(*event);

        while (history.size() > m_options.history_per_thread)
            history.pop_front();
    }

    for (std::size_t slot { 0 }; slot < batches.size(); ++slot)
    {
        batches[slot].set_dropped(dropped[slot]);

        if (batches[slot].events_size() == 0 && batches[slot].dropped() == 0)
            continue;

        pb::ServerMessage message;
        *message.mutable_batch() = batches[slot];

        out.push_back(serialize(message));
    }

    return out;
}

example_commands*
server::commands_for(std::string_view name)
{
    std::lock_guard<std::mutex> lock { m_mutex };

    const auto found { index_of_locked(name) };

    return found.has_value() ? m_commands[*found].get() : nullptr;
}

void
server::handle_client_frame(std::uint64_t client, std::string_view frame)
{
    pb::ClientMessage message;

    if (!message.ParseFromArray(frame.data(), static_cast<int>(frame.size())))
        return;

    // Capture is global across examples: pausing then switching must not
    // silently resume the new example.
    if (message.has_pause())
    {
        m_capture.store(false, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock { m_mutex };

        // Only the active example's recorder owns a timeline; the invariant
        // turns the global switch off without touching dormant recorders
        // (they must stay disabled, see sync_recorder_capture).
        sync_recorder_capture();
    }
    else if (message.has_resume())
    {
        m_capture.store(true, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock { m_mutex };

        // Resuming re-arms the active example only: enabling the dormant
        // recorders here would strand their payloads in undrained queues.
        sync_recorder_capture();
    }
    else if (message.has_select_example())
    {
        // Flip, quarantine, then broadcast the new graph to every client: the
        // page resets its whole view on any graph message, so all timelines
        // restart together. Same loop thread throughout, so the graph can
        // never interleave with batches.
        if (select_example(message.select_example().name()) && m_app != nullptr)
        {
            m_app->publish("events", snapshot(), uWS::OpCode::BINARY, false);

            std::lock_guard<std::mutex> lock { m_mutex };
            m_broadcast_active = active_name_locked();
        }
    }
    else if (message.has_set_input())
    {
        const auto& set_input { message.set_input() };

        handle_command(
            client,
            set_input.request_id(),
            command_kind::set_input,
            set_input.port(),
            set_input.has_example() ? &set_input.example() : nullptr,
            &set_input.value());
    }
    else if (message.has_read_input())
    {
        const auto& read { message.read_input() };

        handle_command(
            client,
            read.request_id(),
            command_kind::read_input,
            read.port(),
            read.has_example() ? &read.example() : nullptr,
            nullptr);
    }
    else if (message.has_read_output())
    {
        const auto& read { message.read_output() };

        handle_command(
            client,
            read.request_id(),
            command_kind::read_output,
            read.port(),
            read.has_example() ? &read.example() : nullptr,
            nullptr);
    }
}

void
server::handle_command(
    std::uint64_t client,
    std::uint64_t request_id,
    command_kind kind,
    std::string_view port,
    const std::string* example_name,
    const pb::Value* value)
{
    std::lock_guard<std::mutex> lock { m_mutex };

    const auto active { m_active.load(std::memory_order_acquire) };

    if (active >= m_examples.size())
    {
        queue_error_reply(client, request_id, "no example is registered");
        return;
    }

    // Chunk A addresses the active example only. The wire field exists (and is
    // parsed) so per-example queues can land later without breaking clients; an
    // explicit name that is not live is reported rather than silently
    // retargeted, which would make an agent debug the wrong program.
    if (example_name != nullptr && !example_name->empty() && *example_name != m_examples[active]->name)
    {
        queue_error_reply(
            client,
            request_id,
            "example \"" + *example_name + "\" is not active (per-example addressing is deferred)");
        return;
    }

    const auto& directory { m_ports[active] };

    // Validate the name against the example's graph description on the loop
    // thread (AS-A3): a bad command costs the tick thread nothing and is
    // answered immediately, with no queue entry.
    const port_endpoint* endpoint
    {
        kind == command_kind::read_output ? directory.find_output(port) : directory.find_input(port)
    };

    if (endpoint == nullptr)
    {
        const bool kind_mismatch
        {
            kind == command_kind::read_output
                ? directory.find_input(port) != nullptr
                : directory.find_output(port) != nullptr
        };

        if (kind_mismatch)
        {
            queue_error_reply(
                client,
                request_id,
                "port \"" + std::string { port } + "\" is "
                    + (kind == command_kind::read_output ? "an input" : "an output")
                    + (kind == command_kind::read_output
                           ? "; ReadOutput targets output ports"
                           : "; SetInput/ReadInput target input ports"));
        }
        else
        {
            queue_error_reply(
                client,
                request_id,
                "unknown " + std::string { kind == command_kind::read_output ? "output" : "input" }
                    + " port \"" + std::string { port } + "\"");
        }

        return;
    }

    command entry;
    entry.connection = client;
    entry.request_id = request_id;
    entry.kind = kind;
    entry.port = endpoint->index;

    if (value != nullptr)
    {
        std::string_view error {};

        const auto decoded { command_value_from_message(*value, error) };

        if (!decoded.has_value())
        {
            // Chunk A defers struct/text injection; the message names the port
            // so a client can surface it verbatim.
            queue_error_reply(
                client,
                request_id,
                "unsupported value kind for port \"" + std::string { port } + "\": "
                    + std::string { error });
            return;
        }

        entry.value = *decoded;
    }

    // A full region queue drops the command here: counted (CommandStats), never
    // acked — a lost command must be visibly lost (AS-A4).
    m_commands[active]->enqueue(endpoint->clock, entry);
}

void
server::queue_error_reply(std::uint64_t client, std::uint64_t request_id, std::string message)
{
    pb::ServerMessage reply;
    auto* ack { reply.mutable_ack() };

    ack->set_request_id(request_id);
    ack->set_error(std::move(message));
    ack->set_applied_epoch(0);

    m_reply_outbox.push_back(outbound_frame { client, serialize(reply) });
}

std::string
server::command_stats_frame() const
{
    std::lock_guard<std::mutex> lock { m_mutex };

    std::uint64_t dropped { 0 };

    if (!m_examples.empty())
        dropped = m_commands[m_active.load(std::memory_order_acquire)]->dropped();

    pb::ServerMessage message;
    message.mutable_command_stats()->set_dropped(dropped);

    return serialize(message);
}

server::command_outbox
server::drain_commands()
{
    std::lock_guard<std::mutex> lock { m_mutex };

    command_outbox out;

    // Immediate validation errors came from this thread, so they leave first
    // (a client sees its own rejects in request order).
    out.replies = std::move(m_reply_outbox);
    m_reply_outbox.clear();

    if (m_examples.empty())
        return out;

    const auto active { m_active.load(std::memory_order_acquire) };

    // Ship replies for every registered example, not just the active one: a
    // tick thread can post a reply just before a switch lands, and a queued
    // reply owns a reference (a traced struct's cell, if the port carries one)
    // that must be handed back promptly rather than sit in a dormant example's
    // queue. The port name resolves against the example that produced it.
    reply incoming;

    for (std::size_t index { 0 }; index < m_examples.size(); ++index)
    {
        const auto& directory { m_ports[index] };

        while (m_commands[index]->take_reply(incoming))
        {
            if (incoming.kind == command_kind::set_input)
            {
                out.replies.push_back(
                    outbound_frame {
                        incoming.connection,
                        ack_frame(incoming.request_id, incoming.error, incoming.applied_epoch) });
            }
            else
            {
                const port_endpoint* endpoint
                {
                    incoming.kind == command_kind::read_input
                        ? directory.find_input_index(incoming.port)
                        : directory.find_output_index(incoming.port)
                };

                out.replies.push_back(
                    outbound_frame {
                        incoming.connection,
                        port_value_frame(
                            incoming.request_id,
                            endpoint != nullptr ? std::string_view { endpoint->name } : std::string_view {},
                            incoming.error,
                            incoming.value) });
            }

            // The reply owned one reference to its snapshot for the crossing;
            // encoding has copied it.
            kernel::release(incoming.value);
        }
    }

    // CommandStats: cumulative, broadcast only when it moves. A lost command is
    // never acked, so this is the client's only signal that its request id will
    // never be answered.
    const auto dropped { m_commands[active]->dropped() };

    if (dropped != m_broadcast_dropped)
    {
        m_broadcast_dropped = dropped;

        pb::ServerMessage message;
        message.mutable_command_stats()->set_dropped(dropped);

        out.stats = serialize(message);
    }

    return out;
}

void
server::stop()
{
    m_stopping.store(true, std::memory_order_release);

    // us_wakeup_loop is the only fully thread-safe call: it just wakes the
    // loop so a stop requested between timer ticks is noticed promptly (the
    // teardown still happens on the loop thread). Guarded by m_running so a
    // stop after run() returned never touches a dead loop.
    if (m_running.load(std::memory_order_acquire))
        if (auto* loop { m_loop.load(std::memory_order_acquire) })
            us_wakeup_loop(loop);
}

void
server::on_timer_tick(us_timer_t* timer)
{
    if (m_stopping.load(std::memory_order_acquire))
    {
        // Loop-thread teardown, in full: drop every client, the timer itself
        // (closing a timer from its own callback is the documented pattern),
        // and the listen socket. With no polls left the loop exits and run()
        // returns.
        for (auto& client : m_connections)
            if (client.socket != nullptr)
                static_cast<socket_type*>(client.socket)->close();

        m_connections.clear();

        auto* context { static_cast<timer_context*>(us_timer_ext(timer)) };

        if (context != nullptr && context->listen_socket != nullptr)
        {
            us_listen_socket_close(0, context->listen_socket);
            context->listen_socket = nullptr;
        }

        us_timer_close(timer);
        return;
    }

    if (m_app == nullptr)
        return;

    // Dynamic examples: the set can change from another thread between ticks
    // (see add_example/remove_example). Broadcast the list once per change, and
    // resend the graph whenever the live example moved.
    const auto revision { m_revision.load(std::memory_order_acquire) };
    std::string active;
    bool list_changed { false };

    {
        std::lock_guard<std::mutex> lock { m_mutex };
        active = active_name_locked();
        list_changed = revision != m_broadcast_revision;
    }

    if (list_changed)
    {
        m_broadcast_revision = revision;
        m_broadcast_active = active;
        m_app->publish("events", examples_snapshot(), uWS::OpCode::BINARY, false);
        m_app->publish("events", snapshot(), uWS::OpCode::BINARY, false);
    }
    else if (active != m_broadcast_active)
    {
        m_broadcast_active = active;
        m_app->publish("events", snapshot(), uWS::OpCode::BINARY, false);
    }

    for (auto& batch : drain())
        m_app->publish("events", std::move(batch), uWS::OpCode::BINARY, false);

    // The write-back command path: replies go to exactly the connection that
    // asked (request ids mean nothing elsewhere), the cumulative drop count is
    // broadcast to every client — the web page ignores the message it does not
    // know.
    auto commands { drain_commands() };

    for (auto& outgoing : commands.replies)
    {
        for (auto& client : m_connections)
        {
            if (client.token != outgoing.connection)
                continue;

            if (client.socket != nullptr)
                static_cast<socket_type*>(client.socket)->send(
                    std::move(outgoing.frame), uWS::OpCode::BINARY, false);

            break;
        }

        // A reply whose connection closed is dropped here; the reference it
        // did not need any more was released while encoding. Tokens are never
        // reused, so a late reply cannot reach the wrong client.
    }

    if (commands.stats.has_value())
        m_app->publish("events", std::move(*commands.stats), uWS::OpCode::BINARY, false);
}

void
server::run()
{
    us_listen_socket_t* listen_socket { nullptr };

    auto app
    {
        uWS::App()
            .get(
                "/*",
                [this](auto* res, auto* req)
                {
                    std::string path { req->getUrl() };

                    if (path == "/")
                        path = "/index.html";

                    const auto file { read_file(m_options.web_root + path) };

                    if (file.empty())
                    {
                        res->writeStatus("404 Not Found")->end("not found");
                        return;
                    }

                    res->writeHeader("Content-Type", content_type(path))->end(file);
                }
            )
            .ws<per_socket_data>(
                "/ws",
                {
                    .compression = uWS::DISABLED,
                    .maxPayloadLength = 64 * 1024,
                    .idleTimeout = 120,
                    .maxBackpressure = 4 * 1024 * 1024,
                    .closeOnBackpressureLimit = false,
                    .sendPingsAutomatically = true,
                    .open =
                        [this](auto* ws)
                        {
                            // Assign the connection's opaque token (never
                            // reused) and register it for reply routing.
                            const auto token { ++m_next_connection };
                            ws->getUserData()->token = token;
                            m_connections.push_back(connection { token, ws });

                            ws->subscribe("events");
                            ws->send(examples_snapshot(), uWS::OpCode::BINARY, true);

                            const auto graph { snapshot() };
                            if (!graph.empty())
                                ws->send(graph, uWS::OpCode::BINARY, true);

                            // Tell a late client the cumulative command-path
                            // loss it missed (the page ignores the message).
                            ws->send(command_stats_frame(), uWS::OpCode::BINARY, true);

                            // This connection is now current; keep the drain
                            // tick from re-broadcasting the same state.
                            m_broadcast_revision = m_revision.load(std::memory_order_acquire);
                            {
                                std::lock_guard<std::mutex> lock { m_mutex };
                                m_broadcast_active = active_name_locked();
                            }

                            // Replay the history tail, chunked so no single
                            // frame grows unbounded.
                            constexpr std::size_t chunk_size { 4096 };

                            for (const auto& history : m_history)
                            {
                                for (std::size_t offset { 0 }; offset < history.size(); offset += chunk_size)
                                {
                                    pb::ServerMessage message;
                                    auto* batch { message.mutable_batch() };

                                    batch->set_clock("");
                                    batch->set_dropped(0);

                                    const auto end { std::min(offset + chunk_size, history.size()) };
                                    for (std::size_t i { offset }; i < end; ++i)
                                        batch->add_events()->CopyFrom(history[i]);

                                    ws->send(serialize(message), uWS::OpCode::BINARY, true);
                                }
                            }
                        },
                    .message =
                        [this](auto* ws, std::string_view data, uWS::OpCode)
                        {
                            // One decode site: the same entry point tests call
                            // directly, so the socket wrapper cannot drift from
                            // the exercised behaviour.
                            handle_client_frame(ws->getUserData()->token, data);
                        },
                    .drain = [](auto*) {},
                    .ping = [](auto*, std::string_view) {},
                    .pong = [](auto*, std::string_view) {},
                    .close =
                        [this](auto* ws, int, std::string_view)
                        {
                            std::erase_if(
                                m_connections,
                                [ws](const connection& client)
                                {
                                    return client.socket == static_cast<void*>(ws);
                                });
                        },
                }
            )
            .listen(
                m_options.host,
                m_options.port,
                // Exclusive: without it the socket takes SO_REUSEPORT and two
                // servers can bind the same port, silently splitting clients
                // between two timelines. An embedded host whose port is taken
                // must fail fast instead (see run()).
                LIBUS_LISTEN_EXCLUSIVE_PORT,
                [&listen_socket, this](auto* token)
                {
                    listen_socket = token;

                    if (token)
                        std::cout << "clockr debugger listening on http://"
                                  << m_options.host << ":" << m_options.port
                                  << " (web root " << m_options.web_root << ")\n";
                    else
                        std::cerr << "clockr debugger failed to listen on "
                                  << m_options.host << ":" << m_options.port << "\n";
                }
            )
    };

    m_app = &app;

    auto* loop { reinterpret_cast<us_loop_t*>(uWS::Loop::get()) };
    m_loop.store(loop, std::memory_order_release);
    m_running.store(true, std::memory_order_release);

    auto* timer { us_create_timer(loop, 0, sizeof(timer_context)) };

    if (timer != nullptr)
    {
        auto* context { static_cast<timer_context*>(us_timer_ext(timer)) };
        context->self = this;
        context->listen_socket = listen_socket;

        us_timer_set(
            timer,
            on_timer,
            static_cast<int>(m_options.drain_period_ms),
            static_cast<int>(m_options.drain_period_ms));
    }

    app.run();

    m_running.store(false, std::memory_order_release);
    m_loop.store(nullptr, std::memory_order_release);
    m_app = nullptr;
}

} // namespace clockr::debug
