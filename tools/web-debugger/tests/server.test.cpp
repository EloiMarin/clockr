#include "server/message.hpp"
#include "server/recorder.hpp"
#include "server/server.hpp"

#include "tracked.hpp"

#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/program.hpp"

#include "clockr/shell/recipe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace clockr;

namespace
{
    using debug_test::tracked;

    // Two single-passthrough programs with distinct port names so their graph
    // descriptions differ, each with its own recorder, behind one server.
    // Members stay in dependency order: the server borrows the programs and
    // recorders, which must outlive it.
    struct two_example_server
    {
        kernel::program first {
            shell::input<std::int64_t, kernel::clock_tag::ui>("a_in")
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("a_out")
        };
        kernel::program second {
            shell::input<std::int64_t, kernel::clock_tag::ui>("b_in")
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("b_out")
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
    };
} // namespace

TEST_CASE("debugger/server | Draining releases traced struct references", "[debugger][server]")
{
    using kernel::clock_tag;

    const auto network
    {
        shell::input<std::int64_t, clock_tag::ui>("n") >> shell::output<std::int64_t, clock_tag::ui>("out")
    };
    kernel::program program { network };

    debug::trace_recorder recorder;
    debug::server server { { { "only", program, recorder } }, {} };

    int destroyed { 0 };

    const auto payload { kernel::make_user_value<tracked>(nullptr, &destroyed) };
    REQUIRE(std::holds_alternative<kernel::user_ref>(payload));

    recorder.on_event(kernel::trace_event {
        kernel::trace_event_kind::node_tick_end,
        clock_tag::ui,
        1,
        0,
        1,
        payload,
        0,
    });

    const auto frames { server.drain() };

    // The event reached a batch, and draining spent the recorder's reference.
    REQUIRE(frames.size() == 1);
    REQUIRE(destroyed == 0);

    kernel::release(payload);
    REQUIRE(destroyed == 1);
}

namespace
{
    kernel::trace_event
    ui_event(std::uint64_t sequence)
    {
        return kernel::trace_event {
            kernel::trace_event_kind::node_tick_end,
            kernel::clock_tag::ui,
            sequence,
            0,
            1,
            kernel::value { std::int64_t { 1 } },
            0,
        };
    }
} // namespace

TEST_CASE("debugger/server | Switching examples swaps the snapshot", "[debugger][server]")
{
    two_example_server fixtures;
    auto& server { fixtures.server };

    REQUIRE(server.active() == 0);
    REQUIRE(server.index_of("first") == 0);
    REQUIRE(server.index_of("second") == 1);
    REQUIRE(!server.index_of("missing").has_value());

    const auto before { server.snapshot() };

    REQUIRE(server.select_example("second"));
    REQUIRE(server.active() == 1);
    REQUIRE(server.snapshot() != before);

    // Re-selecting the live example and unknown names are no-ops: no reset,
    // no new graph.
    REQUIRE(!server.select_example("second"));
    REQUIRE(!server.select_example("missing"));
    REQUIRE(server.active() == 1);
    REQUIRE(server.snapshot() != before);
}

TEST_CASE("debugger/server | Examples can be added and removed at runtime", "[debugger][server]")
{
    kernel::program first {
        shell::input<std::int64_t, kernel::clock_tag::ui>("a_in")
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("a_out")
    };
    kernel::program second {
        shell::input<std::int64_t, kernel::clock_tag::ui>("b_in")
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("b_out")
    };
    debug::trace_recorder first_recorder;
    debug::trace_recorder second_recorder;

    debug::server server { { { "first", first, first_recorder } }, {} };

    REQUIRE(server.example_count() == 1);
    REQUIRE(server.index_of("first") == 0);
    REQUIRE(server.active() == 0);

    server.add_example({ "second", second, second_recorder });

    REQUIRE(server.example_count() == 2);
    REQUIRE(server.index_of("second") == 1);

    // The ExampleList carries every name plus the live one.
    const auto frame { server.examples_snapshot() };
    debug::pb::ServerMessage message;
    REQUIRE(message.ParseFromArray(frame.data(), static_cast<int>(frame.size())));
    REQUIRE(message.has_examples());
    REQUIRE(message.examples().names_size() == 2);
    REQUIRE(message.examples().names(0) == "first");
    REQUIRE(message.examples().names(1) == "second");
    REQUIRE(message.examples().active() == "first");

    // Removing the active example falls back to the one that shifts into its
    // place; removing an absent one is a no-op.
    REQUIRE(server.remove_example("first"));
    REQUIRE(server.example_count() == 1);
    REQUIRE(!server.index_of("first").has_value());
    REQUIRE(server.active() == 0);
    REQUIRE(server.index_of("second") == 0);
    REQUIRE(!server.remove_example("first"));
}

TEST_CASE("debugger/server | An empty server accepts examples later", "[debugger][server]")
{
    kernel::program only {
        shell::input<std::int64_t, kernel::clock_tag::ui>("n")
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };
    debug::trace_recorder recorder;

    debug::server server { {}, {} };

    REQUIRE(server.example_count() == 0);
    REQUIRE(server.snapshot().empty());
    REQUIRE(server.drain().empty());

    server.add_example({ "only", only, recorder });

    REQUIRE(server.example_count() == 1);
    REQUIRE(server.active() == 0);
    REQUIRE(!server.snapshot().empty());
}

TEST_CASE("debugger/server | Switching quarantines the old timeline", "[debugger][server]")
{
    two_example_server fixtures;
    auto& server { fixtures.server };
    auto& first_recorder { fixtures.first_recorder };
    auto& second_recorder { fixtures.second_recorder };

    // An event recorded but not yet drained when the switch lands must never
    // surface: the drain after the switch comes back empty.
    first_recorder.on_event(ui_event(1));
    REQUIRE(server.select_example("second"));
    REQUIRE(server.drain().empty());

    // ... and the newly-active recorder flows normally from a clean slate.
    second_recorder.on_event(ui_event(2));
    const auto frames { server.drain() };
    REQUIRE(frames.size() == 1);
}

TEST_CASE(
    "debugger/server | Only the active example's recorder captures",
    "[debugger][server]")
{
    // drain() consumes exactly one recorder — the active example's — so a
    // dormant recorder that captures strands one user-cell reference per
    // traced payload in a queue nothing drains, and the dormant example's
    // own pools exhaust under its ticker (the precondition of the pool test
    // below). The invariant is re-imposed wherever active-ness changes.
    two_example_server fixtures;
    auto& server { fixtures.server };
    auto& first_recorder { fixtures.first_recorder };
    auto& second_recorder { fixtures.second_recorder };

    REQUIRE(first_recorder.enabled());
    REQUIRE(!second_recorder.enabled());

    // A switch hands capture over to the newly-active example — with the
    // flip's quarantine draining both recorders first.
    REQUIRE(server.select_example("second"));
    REQUIRE(second_recorder.enabled());
    REQUIRE(!first_recorder.enabled());
    REQUIRE(server.drain().empty());

    // Registering into a non-empty server leaves the new example dormant.
    kernel::program third {
        shell::input<std::int64_t, kernel::clock_tag::ui>("c_in")
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("c_out")
    };
    debug::trace_recorder third_recorder;

    server.add_example({ "third", third, third_recorder });

    REQUIRE(!third_recorder.enabled());
    REQUIRE(second_recorder.enabled());

    // Removing the active example hands capture to its neighbour — the
    // element that shifts into the active slot, as remove_example documents.
    REQUIRE(server.remove_example("second"));
    REQUIRE(server.active() == 1);
    REQUIRE(server.index_of("third") == 1);
    REQUIRE(third_recorder.enabled());
    REQUIRE(!first_recorder.enabled());
}

TEST_CASE(
    "debugger/server | A dormant example never retains traced payloads",
    "[debugger][server]")
{
    // Embedded-host regression (an idle debugger drained a second yang
    // instance's ui region dry): with capture correctly dormant, an example
    // that only ever appears as inactive may be stepped forever without a
    // single drain, even on a least-options pool. Each tick hands a 24-byte
    // struct through the passthrough, so the engine churns pool cells every
    // step; retention of one or two would exhaust the 16 KiB budget.
    struct pooled
    {
        std::int64_t a { 0 };
        std::int64_t b { 0 };
        std::int64_t c { 0 };
    };

    kernel::program active {
        shell::input<std::int64_t, kernel::clock_tag::ui>("a_in")
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("a_out")
    };
    kernel::program dormant {
        shell::input<pooled, kernel::clock_tag::ui>("p")
            >> shell::output<pooled, kernel::clock_tag::ui>("out"),
        kernel::program::options { .region_pool_bytes = 16 * 1024 },
    };
    debug::trace_recorder active_recorder;
    debug::trace_recorder dormant_recorder;
    debug::server server {
        {
            { "active", active, active_recorder },
            { "dormant", dormant, dormant_recorder },
        },
        {},
    };

    REQUIRE(!dormant_recorder.enabled());

    kernel::debug_engine engine { dormant_recorder };

    for (int i { 0 }; i < 10'000; ++i)
    {
        dormant.set<pooled>("p", pooled { i, i * 2, i * 3 });
        engine.step(dormant, kernel::clock_tag::ui);
    }

    REQUIRE(kernel::get<pooled>(dormant, "out").a == 9'999);

    // The dormant recorder captured nothing: its queue is empty.
    std::vector<debug::recorded_event> drained;
    REQUIRE(dormant_recorder.drain(drained, 4096) == 0);
}

namespace
{
    // A live server around a single passthrough program. run() blocks the
    // calling thread; stop() unblocks it. Ports are fixed test ports, far from
    // the demo default, so the suite never fights a running debugger_demo.
    struct live_server
    {
        kernel::program program {
            shell::input<std::int64_t, kernel::clock_tag::ui>("n")
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
        };
        debug::trace_recorder recorder;
        debug::server server {
            { { "only", program, recorder } },
            { .host = "127.0.0.1", .port = 18231, .drain_period_ms = 5 },
        };
    };
} // namespace

TEST_CASE("debugger/server | Stopping a live server returns from run", "[debugger][server]")
{
    live_server fixtures;

    std::thread thread { [&] { fixtures.server.run(); } };

    // Let it bind (or fail fast); either way the join below must complete.
    std::this_thread::sleep_for(std::chrono::milliseconds { 300 });
    fixtures.server.stop();
    thread.join();
}

TEST_CASE("debugger/server | A second server on the same port fails gracefully", "[debugger][server]")
{
    live_server first;

    std::thread first_thread { [&] { first.server.run(); } };
    std::this_thread::sleep_for(std::chrono::milliseconds { 300 });

    live_server second;

    // The loser's run() returns instead of blocking: an embedded host whose
    // port is taken simply serves nothing. stop() first so the join below
    // completes even if the platform keeps a failed listen alive.
    std::thread second_thread { [&] { second.server.run(); } };
    std::this_thread::sleep_for(std::chrono::milliseconds { 300 });
    second.server.stop();
    second_thread.join();

    first.server.stop();
    first_thread.join();
}
