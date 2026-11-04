// The write-back command path: wire codec, boundary application, enqueue-time
// validation and visible-drop accounting. The tests drive the same loop-level
// entry points the uWS event loop uses (server::handle_client_frame,
// server::drain_commands). Most cases use the real host path — one
// `command_hook` attached to the engine, then plain `engine.step(...)` — while
// the primitive-level cases call `command_drain`/`command_finish` directly to
// pin queue and backpressure behaviour. No socket is opened.

#include "server/commands.hpp"
#include "server/message.hpp"
#include "server/recorder.hpp"
#include "server/server.hpp"

#include "tracked.hpp"

#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"

#include "clockr/shell/recipe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace clockr;

namespace
{
// The passthrough network the injection cases drive: `level` is the input a
// command writes, `out` mirrors it after a tick, so one boundary exercises a
// write all the way through evaluation.
auto
passthrough_network()
{
    return shell::input<double, kernel::clock_tag::ui>("level") >>
           shell::output<double, kernel::clock_tag::ui>("out");
}

// The epoch a region published for the step just run — what a host (or a test
// using the primitives) passes to command_finish. The step hook gets the same
// value from the engine, throwing path included.
conc::epoch_id
published_epoch(const kernel::program& program, kernel::clock_tag clock)
{
    return program.region(clock).published_epoch.load(
        std::memory_order_acquire
    );
}

// A passthrough program on the ui region: `level` is visible at the
// boundary before a step and `out` mirrors it after, so one
// drain/step/finish cycle exercises both directions of the command path.
struct boundary_fixture
{
    kernel::program program { passthrough_network() };
    debug::trace_recorder recorder;
    debug::server server {
        { { "only", program, recorder } },
        { .command_queue_capacity = 4 },
    };
    debug::example_commands& commands { *server.commands_for("only") };
};

std::string
wire_bytes(const debug::pb::ClientMessage& message)
{
    std::string bytes;
    message.SerializeToString(&bytes);
    return bytes;
}

debug::pb::Value
double_value(double value)
{
    debug::pb::Value message;
    message.set_double_value(value);
    return message;
}

debug::pb::ClientMessage
set_input_frame(
    std::uint64_t request_id,
    std::string_view port,
    const debug::pb::Value& value,
    std::optional<std::string_view> example = std::nullopt
)
{
    debug::pb::ClientMessage message;
    auto* input { message.mutable_set_input() };

    input->set_request_id(request_id);
    input->set_port(std::string { port });
    *input->mutable_value() = value;

    if (example.has_value())
        input->set_example(std::string { *example });

    return message;
}

debug::pb::ClientMessage
read_frame(
    std::uint64_t request_id,
    std::string_view port,
    std::optional<std::string_view> example = std::nullopt
)
{
    debug::pb::ClientMessage message;
    auto* read { message.mutable_read_input() };

    read->set_request_id(request_id);
    read->set_port(std::string { port });

    if (example.has_value())
        read->set_example(std::string { *example });

    return message;
}

debug::pb::ClientMessage
read_output_frame(
    std::uint64_t request_id,
    std::string_view port,
    std::optional<std::string_view> example = std::nullopt
)
{
    debug::pb::ClientMessage message;
    auto* read { message.mutable_read_output() };

    read->set_request_id(request_id);
    read->set_port(std::string { port });

    if (example.has_value())
        read->set_example(std::string { *example });

    return message;
}

// One frame as it left the server, decoded back into its wire message.
struct decoded_reply
{
    std::uint64_t connection { 0 };
    debug::pb::ServerMessage message;
};

std::vector<decoded_reply>
decode(const std::vector<debug::outbound_frame>& frames)
{
    std::vector<decoded_reply> decoded;
    decoded.reserve(frames.size());

    for (const auto& frame : frames)
    {
        decoded_reply reply;
        reply.connection = frame.connection;
        reply.message.ParseFromString(frame.frame);
        decoded.push_back(std::move(reply));
    }

    return decoded;
}
} // namespace

TEST_CASE(
    "debugger.schema.setinput | SetInput/ReadInput/ReadOutput/Ack/PortValue "
    "round-trip",
    "[debugger][schema]"
)
{
    // ClientMessage { set_input = 4 }: the request correlation id, the port
    // name, the scalar payload and the optional example addressing field all
    // survive the wire.
    {
        const auto bytes {
            wire_bytes(set_input_frame(42, "bpm", double_value(123.0), "demo"))
        };

        debug::pb::ClientMessage decoded;
        REQUIRE(
            decoded.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
        );
        REQUIRE(decoded.has_set_input());
        REQUIRE(decoded.set_input().request_id() == 42);
        REQUIRE(decoded.set_input().port() == "bpm");
        REQUIRE(
            decoded.set_input().value().kind_case() ==
            debug::pb::Value::kDoubleValue
        );
        REQUIRE(decoded.set_input().value().double_value() == 123.0);
        REQUIRE(decoded.set_input().has_example());
        REQUIRE(decoded.set_input().example() == "demo");
    }

    // The example field is proto3-optional: "addressed to a name" and
    // "unaddressed (use the active example)" stay distinguishable.
    {
        const auto bytes {
            wire_bytes(set_input_frame(1, "bpm", double_value(1.0)))
        };

        debug::pb::ClientMessage decoded;
        REQUIRE(
            decoded.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
        );
        REQUIRE(decoded.has_set_input());
        REQUIRE(!decoded.set_input().has_example());
    }

    // ReadInput (field 5) and ReadOutput (field 6) are distinct oneof members.
    {
        const auto input_bytes { wire_bytes(read_frame(7, "bpm")) };
        debug::pb::ClientMessage input;
        REQUIRE(input.ParseFromArray(
            input_bytes.data(), static_cast<int>(input_bytes.size())
        ));
        REQUIRE(input.has_read_input());
        REQUIRE(!input.has_set_input());
        REQUIRE(!input.has_read_output());
        REQUIRE(input.read_input().request_id() == 7);
        REQUIRE(input.read_input().port() == "bpm");

        const auto output_bytes {
            wire_bytes(read_output_frame(8, "tempo", "demo"))
        };
        debug::pb::ClientMessage output;
        REQUIRE(output.ParseFromArray(
            output_bytes.data(), static_cast<int>(output_bytes.size())
        ));
        REQUIRE(output.has_read_output());
        REQUIRE(!output.has_read_input());
        REQUIRE(output.read_output().request_id() == 8);
        REQUIRE(output.read_output().port() == "tempo");
        REQUIRE(output.read_output().has_example());
        REQUIRE(output.read_output().example() == "demo");
    }

    // ServerMessage { ack = 4 }: a clean apply carries only the epoch, an error
    // carries only the error text (and applied_epoch 0, which proto3 omits).
    {
        debug::pb::ServerMessage message;
        message.mutable_ack()->set_request_id(9);
        message.mutable_ack()->set_applied_epoch(17);

        std::string bytes;
        message.SerializeToString(&bytes);

        debug::pb::ServerMessage decoded;
        REQUIRE(
            decoded.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
        );
        REQUIRE(decoded.has_ack());
        REQUIRE(decoded.ack().request_id() == 9);
        REQUIRE(decoded.ack().error().empty());
        REQUIRE(decoded.ack().applied_epoch() == 17);

        debug::pb::ServerMessage error;
        error.mutable_ack()->set_request_id(10);
        error.mutable_ack()->set_error("unknown input port \"nope\"");

        std::string error_bytes;
        error.SerializeToString(&error_bytes);

        debug::pb::ServerMessage decoded_error;
        REQUIRE(decoded_error.ParseFromArray(
            error_bytes.data(), static_cast<int>(error_bytes.size())
        ));
        REQUIRE(decoded_error.ack().error() == "unknown input port \"nope\"");
        REQUIRE(decoded_error.ack().applied_epoch() == 0);
    }

    // ServerMessage { port_value = 5 }: the snapshot travels in the same Value
    // form the trace uses.
    {
        debug::pb::ServerMessage message;
        auto* answer { message.mutable_port_value() };

        answer->set_request_id(11);
        answer->set_port("out");
        answer->mutable_value()->set_double_value(2.5);

        std::string bytes;
        message.SerializeToString(&bytes);

        debug::pb::ServerMessage decoded;
        REQUIRE(
            decoded.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
        );
        REQUIRE(decoded.has_port_value());
        REQUIRE(decoded.port_value().request_id() == 11);
        REQUIRE(decoded.port_value().port() == "out");
        REQUIRE(decoded.port_value().error().empty());
        REQUIRE(
            decoded.port_value().value().kind_case() ==
            debug::pb::Value::kDoubleValue
        );
        REQUIRE(decoded.port_value().value().double_value() == 2.5);
    }

    // ServerMessage { command_stats = 6 }.
    {
        debug::pb::ServerMessage message;
        message.mutable_command_stats()->set_dropped(3);

        std::string bytes;
        message.SerializeToString(&bytes);

        debug::pb::ServerMessage decoded;
        REQUIRE(
            decoded.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
        );
        REQUIRE(decoded.has_command_stats());
        REQUIRE(decoded.command_stats().dropped() == 3);
    }

    // The injection codec accepts exactly the scalar kernel payloads; an
    // absent Value means monostate, and struct/text are rejected with a message
    // (the deferred typed-setter path).
    {
        debug::pb::Value value;
        value.set_bool_value(true);

        std::string_view error;
        const auto decoded { debug::command_value_from_message(value, error) };

        REQUIRE(decoded.has_value());
        REQUIRE(decoded->which == debug::command_value::kind::boolean);
        REQUIRE(std::get<bool>(decoded->to_value()) == true);
    }

    {
        debug::pb::Value value;
        value.set_double_value(-1.5);

        std::string_view error;
        const auto decoded { debug::command_value_from_message(value, error) };

        REQUIRE(decoded.has_value());
        REQUIRE(decoded->which == debug::command_value::kind::real);
        REQUIRE(std::get<double>(decoded->to_value()) == -1.5);
    }

    {
        debug::pb::Value absent;

        std::string_view error;
        const auto decoded { debug::command_value_from_message(absent, error) };

        REQUIRE(decoded.has_value());
        REQUIRE(decoded->which == debug::command_value::kind::unit);
        REQUIRE(std::holds_alternative<std::monostate>(decoded->to_value()));
    }

    {
        debug::pb::Value value;
        value.mutable_struct_value()->set_type_id(3);

        std::string_view error;
        REQUIRE(!debug::command_value_from_message(value, error).has_value());
        REQUIRE(error.find("struct") != std::string_view::npos);
    }

    {
        debug::pb::Value value;
        value.set_string_value("hello");

        std::string_view error;
        REQUIRE(!debug::command_value_from_message(value, error).has_value());
        REQUIRE(!error.empty());
    }
}

TEST_CASE(
    "debugger.injection.boundary | Commands are applied at the engine's tick "
    "boundary and acked with its epoch",
    "[debugger][injection]"
)
{
    // The real host path: one command_hook attached to the engine, and a tick
    // loop that is exactly `engine.step(program, clock)`. No per-call-site
    // drain/finish — the hook runs inside the step.
    boundary_fixture fixture;

    debug::command_hook hook { fixture.commands };

    kernel::debug_engine engine { fixture.recorder };
    engine.set_step_hook(&hook);

    // The client asks; the server only enqueues. No Ack can exist before a
    // boundary runs (AS-A2).
    fixture.server.handle_client_frame(
        100, wire_bytes(set_input_frame(7, "level", double_value(3.5)))
    );

    {
        const auto before { fixture.server.drain_commands() };
        REQUIRE(before.replies.empty());
        REQUIRE(!before.stats.has_value());
    }

    engine.step(fixture.program, kernel::clock_tag::ui);

    // The injected value was set between ticks and consumed by this tick: the
    // passthrough output mirrors it and the region published epoch 1.
    REQUIRE(std::get<double>(fixture.program.get("out")) == 3.5);

    const auto published {
        published_epoch(fixture.program, kernel::clock_tag::ui)
    };

    REQUIRE(published == 1);

    // Exactly one targeted Ack: request id echoed, no error, and the epoch of
    // the tick that consumed the command.
    const auto replies { decode(fixture.server.drain_commands().replies) };

    REQUIRE(replies.size() == 1);
    REQUIRE(replies.front().connection == 100);
    REQUIRE(replies.front().message.has_ack());

    const auto& ack { replies.front().message.ack() };
    REQUIRE(ack.request_id() == 7);
    REQUIRE(ack.error().empty());
    REQUIRE(ack.applied_epoch() == published);

    // The parity the plan pins: Ack.applied_epoch is the epoch the trace names
    // for that region tick (REGION_EVAL_END), so an agent can correlate.
    std::vector<debug::recorded_event> traced;
    fixture.recorder.drain(traced, 1024);

    bool region_end_seen { false };

    for (const auto& recorded : traced)
    {
        if (recorded.event.kind != kernel::trace_event_kind::region_step_end)
            continue;

        REQUIRE(recorded.event.epoch == ack.applied_epoch());
        region_end_seen = true;
    }

    REQUIRE(region_end_seen);

    // Reads ride the same hook-driven boundary: ReadOutput reports the last
    // completed tick, ReadInput the pending input value.
    fixture.server.handle_client_frame(
        100, wire_bytes(read_output_frame(8, "out"))
    );
    fixture.server.handle_client_frame(100, wire_bytes(read_frame(9, "level")));

    engine.step(fixture.program, kernel::clock_tag::ui);

    const auto read_replies { decode(fixture.server.drain_commands().replies) };

    REQUIRE(read_replies.size() == 2);

    REQUIRE(read_replies[0].message.has_port_value());
    REQUIRE(read_replies[0].message.port_value().request_id() == 8);
    REQUIRE(read_replies[0].message.port_value().port() == "out");
    REQUIRE(read_replies[0].message.port_value().error().empty());
    REQUIRE(
        read_replies[0].message.port_value().value().kind_case() ==
        debug::pb::Value::kDoubleValue
    );
    REQUIRE(read_replies[0].message.port_value().value().double_value() == 3.5);

    REQUIRE(read_replies[1].message.has_port_value());
    REQUIRE(read_replies[1].message.port_value().request_id() == 9);
    REQUIRE(read_replies[1].message.port_value().port() == "level");
    REQUIRE(read_replies[1].message.port_value().error().empty());
    REQUIRE(read_replies[1].message.port_value().value().double_value() == 3.5);

    // The same host path with a release_engine: injection works with no trace
    // configured at all. The loop line is identical (`engine.step(...)`); only
    // the construction choice differs — stepping a release_engine instead of a
    // debug_engine must not require touching the tick loop.
    kernel::program release_program { passthrough_network() };
    debug::trace_recorder release_recorder;
    debug::server release_server {
        { { "only", release_program, release_recorder } },
        {},
    };

    auto& release_commands { *release_server.commands_for("only") };
    debug::command_hook release_hook { release_commands };

    kernel::release_engine release_engine;
    release_engine.set_step_hook(&release_hook);

    release_server.handle_client_frame(
        200, wire_bytes(set_input_frame(11, "level", double_value(9.0)))
    );

    release_engine.step(release_program, kernel::clock_tag::ui);

    REQUIRE(std::get<double>(release_program.get_input("level")) == 9.0);
    REQUIRE(std::get<double>(release_program.get("out")) == 9.0);

    const auto release_published {
        published_epoch(release_program, kernel::clock_tag::ui)
    };

    REQUIRE(release_published == 1);

    const auto release_acks { decode(release_server.drain_commands().replies) };

    REQUIRE(release_acks.size() == 1);
    REQUIRE(release_acks.front().connection == 200);
    REQUIRE(release_acks.front().message.has_ack());
    REQUIRE(release_acks.front().message.ack().request_id() == 11);
    REQUIRE(release_acks.front().message.ack().error().empty());
    REQUIRE(
        release_acks.front().message.ack().applied_epoch() == release_published
    );

    // ... and nothing was traced, because there is no debug engine sink.
    std::vector<debug::recorded_event> release_traced;
    REQUIRE(release_recorder.drain(release_traced, 1024) == 0);
}

TEST_CASE(
    "debugger.injection.unknown_port | Invalid commands are answered "
    "immediately and never queued",
    "[debugger][injection]"
)
{
    boundary_fixture fixture;

    // Unknown input name.
    fixture.server.handle_client_frame(
        100, wire_bytes(set_input_frame(1, "nope", double_value(1.0)))
    );

    // An output port addressed by a write: wrong kind, answered on the spot.
    fixture.server.handle_client_frame(
        100, wire_bytes(set_input_frame(2, "out", double_value(1.0)))
    );

    // Unknown output name on a read.
    fixture.server.handle_client_frame(
        100, wire_bytes(read_output_frame(3, "nope"))
    );

    // The active example is the only addressable one in the debug server: an explicit
    // different name is an error, never a silent retarget (deferred).
    fixture.server.handle_client_frame(
        100, wire_bytes(set_input_frame(4, "level", double_value(1.0), "other"))
    );

    // Nothing reached the boundary queue (AS-A3: validated on the loop thread,
    // no per-tick cost).
    {
        debug::command entry;
        REQUIRE(!fixture.commands.receive(kernel::clock_tag::ui, entry));
    }

    const auto replies { decode(fixture.server.drain_commands().replies) };

    REQUIRE(replies.size() == 4);

    for (const auto& reply : replies)
    {
        REQUIRE(reply.connection == 100);
        REQUIRE(reply.message.has_ack());
        REQUIRE(!reply.message.ack().error().empty());
        REQUIRE(reply.message.ack().applied_epoch() == 0);
    }

    // Request ids echo back in the order the commands arrived...
    REQUIRE(replies[0].message.ack().request_id() == 1);
    REQUIRE(replies[1].message.ack().request_id() == 2);
    REQUIRE(replies[2].message.ack().request_id() == 3);
    REQUIRE(replies[3].message.ack().request_id() == 4);

    // ... and each error says what was wrong with that command.
    REQUIRE(
        replies[0].message.ack().error().find("unknown input port \"nope\"") !=
        std::string::npos
    );
    REQUIRE(
        replies[1].message.ack().error().find("port \"out\" is an output") !=
        std::string::npos
    );
    REQUIRE(
        replies[2].message.ack().error().find("unknown output port \"nope\"") !=
        std::string::npos
    );
    REQUIRE(
        replies[3].message.ack().error().find("not active") != std::string::npos
    );

    // A frame that is not valid protobuf is ignored, as before, and answered
    // with nothing at all.
    fixture.server.handle_client_frame(100, std::string_view { "\xff\xff", 2 });

    REQUIRE(fixture.server.drain_commands().replies.empty());
}

TEST_CASE(
    "debugger.injection.drop_visibly | A full command queue drops visibly and "
    "never acks",
    "[debugger][injection]"
)
{
    // Capacity two per region: the third valid command of one drain window
    // cannot be queued.
    kernel::program program {
        shell::input<double, kernel::clock_tag::ui>("level") >>
            shell::output<double, kernel::clock_tag::ui>("out"),
    };
    debug::trace_recorder recorder;
    debug::server server {
        { { "only", program, recorder } },
        { .command_queue_capacity = 2 },
    };

    auto& commands { *server.commands_for("only") };

    kernel::debug_engine engine { recorder };

    for (std::uint64_t id { 1 }; id <= 3; ++id)
    {
        server.handle_client_frame(
            100,
            wire_bytes(set_input_frame(
                id, "level", double_value(static_cast<double>(id))
            ))
        );
    }

    // The lost command is counted (AS-A4), and never acked.
    REQUIRE(commands.dropped() == 1);

    const auto stats_outbox { server.drain_commands() };

    REQUIRE(stats_outbox.replies.empty());
    REQUIRE(stats_outbox.stats.has_value());

    debug::pb::ServerMessage stats;
    REQUIRE(stats.ParseFromString(*stats_outbox.stats));
    REQUIRE(stats.has_command_stats());
    REQUIRE(stats.command_stats().dropped() == 1);

    // Broadcast when the cumulative count moves, not once per drain window.
    REQUIRE(!server.drain_commands().stats.has_value());

    // The boundary applies exactly the two accepted commands; the dropped one
    // is simply absent (never acked, so the client's request id stalls —
    // visibly, because CommandStats moved).
    REQUIRE(
        debug::command_drain(program, kernel::clock_tag::ui, commands) == 2
    );

    engine.step(program, kernel::clock_tag::ui);
    debug::command_finish(
        program,
        kernel::clock_tag::ui,
        commands,
        published_epoch(program, kernel::clock_tag::ui)
    );

    const auto replies { decode(server.drain_commands().replies) };

    REQUIRE(replies.size() == 2);

    std::vector<std::uint64_t> acked;

    for (const auto& reply : replies)
    {
        REQUIRE(reply.message.has_ack());
        REQUIRE(reply.message.ack().error().empty());
        acked.push_back(reply.message.ack().request_id());
    }

    REQUIRE(acked == std::vector<std::uint64_t> { 1, 2 });

    // Both accepted writes were applied at the same boundary; the last one
    // wins, and the lost third never took effect.
    REQUIRE(std::get<double>(program.get("out")) == 2.0);
}

TEST_CASE(
    "debugger.injection.throwing_step | A command consumed by a throwing tick "
    "is acked exactly once",
    "[debugger][injection]"
)
{
    // A tick can abort after the command was already applied. The hook's
    // after_step runs once on the engine's unwind path with threw=true and the
    // attempted epoch, so the Ack is still posted exactly once and names the
    // aborted tick (the one the trace reports as a threw=true
    // REGION_EVAL_END).
    kernel::program program {
        shell::input<double, kernel::clock_tag::ui>("level") >>
            shell::lift<double, double>([](const double& value) {
                if (value > 100.0)
                    throw std::runtime_error { "level out of range" };

                return value;
            }) >>
            shell::output<double, kernel::clock_tag::ui>("out"),
    };
    debug::trace_recorder recorder;
    debug::server server { { { "only", program, recorder } }, {} };

    auto& commands { *server.commands_for("only") };
    debug::command_hook hook { commands };

    kernel::debug_engine engine { recorder };
    engine.set_step_hook(&hook);

    server.handle_client_frame(
        100, wire_bytes(set_input_frame(5, "level", double_value(999.0)))
    );

    REQUIRE_THROWS_AS(
        engine.step(program, kernel::clock_tag::ui), std::runtime_error
    );

    // The throwing tick never published; the hook still reported it, so the
    // Ack names the attempted tick (epoch 1).
    REQUIRE(published_epoch(program, kernel::clock_tag::ui) == 0);

    const auto replies { decode(server.drain_commands().replies) };

    REQUIRE(replies.size() == 1);
    REQUIRE(replies.front().message.has_ack());
    REQUIRE(replies.front().message.ack().request_id() == 5);
    REQUIRE(replies.front().message.ack().error().empty());
    REQUIRE(replies.front().message.ack().applied_epoch() == 1);

    // The aborted tick is visible in the trace under the same epoch, flagged
    // threw=true — the correlation an agent uses after a failed tick.
    {
        std::vector<debug::recorded_event> traced;
        recorder.drain(traced, 1024);

        bool aborted_seen { false };

        for (const auto& recorded : traced)
        {
            if (recorded.event.kind !=
                kernel::trace_event_kind::region_step_end)
                continue;

            REQUIRE(recorded.event.epoch == 1);
            REQUIRE(recorded.event.payload == kernel::value { true });
            aborted_seen = true;
        }

        REQUIRE(aborted_seen);
    }

    // The pending-ack vector did not survive the failed tick: the next
    // boundary acks only its own command, so nothing is double-acked.
    server.handle_client_frame(
        100, wire_bytes(set_input_frame(6, "level", double_value(1.0)))
    );

    engine.step(program, kernel::clock_tag::ui);

    const auto next { decode(server.drain_commands().replies) };

    REQUIRE(next.size() == 1);
    REQUIRE(next.front().message.has_ack());
    REQUIRE(next.front().message.ack().request_id() == 6);
    REQUIRE(next.front().message.ack().error().empty());
    REQUIRE(next.front().message.ack().applied_epoch() == 1);

    REQUIRE(std::get<double>(program.get("out")) == 1.0);
}

TEST_CASE(
    "debugger.injection.remove_example | Removing an example discards its "
    "queues and keeps survivors aligned",
    "[debugger][injection]"
)
{
    // `commands_for` is a borrowed reference into the server's command
    // storage: valid only while the example is registered. `remove_example`
    // discards whatever is queued for it (a command no host will ever apply is
    // dropped, not acked) and the surviving examples' queues must stay paired
    // with their programs/port directories after the parallel arrays shift.
    kernel::program first {
        shell::input<double, kernel::clock_tag::ui>("a_in") >>
            shell::output<double, kernel::clock_tag::ui>("a_out"),
    };
    kernel::program second {
        shell::input<double, kernel::clock_tag::ui>("b_in") >>
            shell::output<double, kernel::clock_tag::ui>("b_out"),
    };
    debug::trace_recorder first_recorder;
    debug::trace_recorder second_recorder;

    debug::server server {
        {
            { "first", first, first_recorder },
            { "second", second, second_recorder },
        },
        {},
    };

    kernel::debug_engine engine { second_recorder };

    // Queue a command for `first`, then unregister it before its ticker runs.
    server.handle_client_frame(
        1, wire_bytes(set_input_frame(1, "a_in", double_value(1.0)))
    );

    REQUIRE(server.remove_example("first"));
    REQUIRE(server.commands_for("first") == nullptr);
    REQUIRE(server.commands_for("second") != nullptr);

    // The discarded command produced no reply (no Ack, no stats movement).
    {
        const auto outbox { server.drain_commands() };
        REQUIRE(outbox.replies.empty());
        REQUIRE(!outbox.stats.has_value());
    }

    // The survivor's queue still routes its own command correctly, and its
    // program is the one that gets stepped.
    auto& second_commands { *server.commands_for("second") };

    server.handle_client_frame(
        2, wire_bytes(set_input_frame(2, "b_in", double_value(2.5)))
    );

    REQUIRE(
        debug::command_drain(second, kernel::clock_tag::ui, second_commands) ==
        1
    );
    REQUIRE(std::get<double>(second.get_input("b_in")) == 2.5);

    engine.step(second, kernel::clock_tag::ui);
    debug::command_finish(
        second,
        kernel::clock_tag::ui,
        second_commands,
        published_epoch(second, kernel::clock_tag::ui)
    );

    const auto replies { decode(server.drain_commands().replies) };

    REQUIRE(replies.size() == 1);
    REQUIRE(replies.front().message.has_ack());
    REQUIRE(replies.front().message.ack().request_id() == 2);
    REQUIRE(replies.front().message.ack().error().empty());
    REQUIRE(replies.front().message.ack().applied_epoch() == 1);
    REQUIRE(std::get<double>(second.get("b_out")) == 2.5);
}

TEST_CASE(
    "debugger.injection.discard_pending | A removed example releases queued "
    "reply references",
    "[debugger][injection]"
)
{
    using debug_test::tracked;

    // A reply that is queued but never shipped owns one reference to its
    // snapshot (a user struct's cell, if the port carries one); discarding the
    // queues must hand it back, exactly like the recorder's destructor. The
    // server runs this on remove_example and on its own destruction.
    int destroyed { 0 };

    const auto payload {
        kernel::make_user_value<tracked>(nullptr, &destroyed)
    };
    REQUIRE(std::holds_alternative<kernel::user_ref>(payload));

    debug::example_commands commands { 4 };

    debug::reply pending;
    pending.connection = 1;
    pending.request_id = 1;
    pending.kind = debug::command_kind::read_output;
    pending.value = payload;
    kernel::retain(pending.value);

    REQUIRE(commands.post(kernel::clock_tag::ui, std::move(pending)));
    REQUIRE(destroyed == 0);

    commands.discard_pending();
    REQUIRE(destroyed == 0); // the test still holds one reference

    kernel::release(payload);
    REQUIRE(destroyed == 1);
}

TEST_CASE(
    "debugger.injection.reply_backpressure | A burst waits for reply slots "
    "instead of overflowing the queue",
    "[debugger][injection]"
)
{
    // The regression this pins: reply capacity equals command capacity, but a
    // tick boundary can consume the whole command queue while the uWS loop
    // only drains replies on its timer, so a burst used to overflow the reply
    // queue (SIGABRT in debug, lost Acks in release). A boundary must now
    // consume only the commands whose replies fit, leaving the rest queued and
    // still answering every accepted command exactly once.
    kernel::program program {
        shell::input<double, kernel::clock_tag::ui>("level") >>
            shell::output<double, kernel::clock_tag::ui>("out"),
    };
    debug::trace_recorder recorder;
    debug::server server {
        { { "only", program, recorder } },
        { .command_queue_capacity = 3 },
    };

    auto& commands { *server.commands_for("only") };

    kernel::debug_engine engine { recorder };

    // First burst: three commands fill the reply queue with three Acks.
    for (std::uint64_t id { 1 }; id <= 3; ++id)
    {
        server.handle_client_frame(
            100,
            wire_bytes(set_input_frame(
                id, "level", double_value(static_cast<double>(id))
            ))
        );
    }

    REQUIRE(
        debug::command_drain(program, kernel::clock_tag::ui, commands) == 3
    );

    engine.step(program, kernel::clock_tag::ui);
    debug::command_finish(
        program,
        kernel::clock_tag::ui,
        commands,
        published_epoch(program, kernel::clock_tag::ui)
    );

    // Second burst arrives and the ticker runs again before the loop shipped
    // anything: no reply slot is free, so this boundary consumes nothing. It
    // must not overflow, and the commands must not be lost.
    for (std::uint64_t id { 4 }; id <= 6; ++id)
    {
        server.handle_client_frame(
            100,
            wire_bytes(set_input_frame(
                id, "level", double_value(static_cast<double>(id))
            ))
        );
    }

    REQUIRE(
        debug::command_drain(program, kernel::clock_tag::ui, commands) == 0
    );

    engine.step(program, kernel::clock_tag::ui);
    debug::command_finish(
        program,
        kernel::clock_tag::ui,
        commands,
        published_epoch(program, kernel::clock_tag::ui)
    );

    // The loop ships the first three Acks, freeing exactly three slots.
    const auto first { decode(server.drain_commands().replies) };

    REQUIRE(first.size() == 3);

    // ... and the waiting commands are consumed and answered by a later
    // boundary.
    REQUIRE(
        debug::command_drain(program, kernel::clock_tag::ui, commands) == 3
    );

    engine.step(program, kernel::clock_tag::ui);
    debug::command_finish(
        program,
        kernel::clock_tag::ui,
        commands,
        published_epoch(program, kernel::clock_tag::ui)
    );

    const auto second { decode(server.drain_commands().replies) };

    REQUIRE(second.size() == 3);

    // Exactly one Ack per accepted request, in per-region order.
    std::vector<std::uint64_t> acked;

    for (const auto& reply : first)
    {
        REQUIRE(reply.message.has_ack());
        REQUIRE(reply.message.ack().error().empty());
        acked.push_back(reply.message.ack().request_id());
    }

    for (const auto& reply : second)
    {
        REQUIRE(reply.message.has_ack());
        REQUIRE(reply.message.ack().error().empty());
        acked.push_back(reply.message.ack().request_id());
    }

    REQUIRE(acked == std::vector<std::uint64_t> { 1, 2, 3, 4, 5, 6 });

    // The deferred commands were applied in order; the last one wins.
    REQUIRE(std::get<double>(program.get("out")) == 6.0);
}

TEST_CASE(
    "debugger.injection.cold_region | A region with no ticker accumulates "
    "commands until its first boundary",
    "[debugger][injection]"
)
{
    // AS-A2: a command aimed at a region nobody ticks is neither an error nor
    // an Ack — it waits in that region's queue. Only the boundary that finally
    // runs consumes it, and it answers once with the epoch of that first tick.
    // (A read on a cold region behaves the same; the deterministic case here
    // is a write so the Ack's epoch is checkable.)
    const auto network { shell::parallel(
        shell::input<double, kernel::clock_tag::ui>("level") >>
            shell::output<double, kernel::clock_tag::ui>("out"),
        shell::input<double, kernel::clock_tag::audio>("audio_level") >>
            shell::output<double, kernel::clock_tag::audio>("audio_out")
    ) };
    kernel::program program { network };

    debug::trace_recorder recorder;
    debug::server server { { { "only", program, recorder } }, {} };

    auto& commands { *server.commands_for("only") };

    kernel::debug_engine audio_engine { recorder };

    server.handle_client_frame(
        100, wire_bytes(set_input_frame(1, "audio_level", double_value(7.5)))
    );

    // The ui boundary must not touch the audio region's queue, and no Ack may
    // be produced while the audio region never ticks.
    REQUIRE(
        debug::command_drain(program, kernel::clock_tag::ui, commands) == 0
    );
    REQUIRE(server.drain_commands().replies.empty());

    // The audio ticker wakes: exactly one boundary consumes the command and
    // answers with the first audio tick's epoch.
    REQUIRE(
        debug::command_drain(program, kernel::clock_tag::audio, commands) == 1
    );
    REQUIRE(std::get<double>(program.get_input("audio_level")) == 7.5);

    audio_engine.step(program, kernel::clock_tag::audio);

    const auto published {
        program.region(kernel::clock_tag::audio)
            .published_epoch.load(std::memory_order_acquire)
    };

    REQUIRE(published == 1);

    debug::command_finish(
        program, kernel::clock_tag::audio, commands, published
    );

    const auto replies { decode(server.drain_commands().replies) };

    REQUIRE(replies.size() == 1);
    REQUIRE(replies.front().message.has_ack());
    REQUIRE(replies.front().message.ack().request_id() == 1);
    REQUIRE(replies.front().message.ack().error().empty());
    REQUIRE(replies.front().message.ack().applied_epoch() == published);

    REQUIRE(std::get<double>(program.get("audio_out")) == 7.5);
}

TEST_CASE(
    "debugger.injection.detach | Clearing the step hook leaves commands queued "
    "until reattached",
    "[debugger][injection]"
)
{
    // Detaching is the kill switch: a host that clears its engine's hook (or
    // never attaches one) accepts commands at the server but applies nothing.
    // The queued command must survive — no loss, no Ack — and be applied once
    // a hook is attached again.
    boundary_fixture fixture;

    debug::command_hook hook { fixture.commands };

    kernel::debug_engine engine { fixture.recorder };
    engine.set_step_hook(&hook);

    // One observed boundary first, so the engine and server are hot.
    fixture.server.handle_client_frame(
        100, wire_bytes(set_input_frame(1, "level", double_value(1.0)))
    );

    engine.step(fixture.program, kernel::clock_tag::ui);

    REQUIRE(std::get<double>(fixture.program.get("out")) == 1.0);
    REQUIRE(decode(fixture.server.drain_commands().replies).size() == 1);

    // Detach: the server still accepts a command, but no boundary consumes it.
    engine.set_step_hook(nullptr);

    REQUIRE(engine.get_step_hook() == nullptr);

    fixture.server.handle_client_frame(
        100, wire_bytes(set_input_frame(2, "level", double_value(2.0)))
    );

    engine.step(fixture.program, kernel::clock_tag::ui);
    engine.step(fixture.program, kernel::clock_tag::ui);

    REQUIRE(std::get<double>(fixture.program.get_input("level")) == 1.0);
    REQUIRE(fixture.server.drain_commands().replies.empty());

    // Reattach: the next boundary drains the queued command, applies it and
    // acks it with the epoch of that tick.
    engine.set_step_hook(&hook);

    engine.step(fixture.program, kernel::clock_tag::ui);

    const auto replies { decode(fixture.server.drain_commands().replies) };

    REQUIRE(replies.size() == 1);
    REQUIRE(replies.front().message.has_ack());
    REQUIRE(replies.front().message.ack().request_id() == 2);
    REQUIRE(replies.front().message.ack().error().empty());
    REQUIRE(
        replies.front().message.ack().applied_epoch() ==
        published_epoch(fixture.program, kernel::clock_tag::ui)
    );

    REQUIRE(std::get<double>(fixture.program.get("out")) == 2.0);
}
