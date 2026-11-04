#include "server/recorder.hpp"

#include "tracked.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace clockr;

namespace
{
    kernel::trace_event
    event(kernel::clock_tag clock, std::uint64_t sequence)
    {
        return kernel::trace_event {
            kernel::trace_event_kind::node_tick_begin,
            clock,
            sequence, // reuse sequence as epoch for test brevity
            sequence,
            sequence,
            kernel::value { std::monostate {} },
            0,
        };
    }
} // namespace

TEST_CASE("debugger/recorder | Drained events keep per-thread FIFO order", "[debugger][recorder]")
{
    debug::trace_recorder recorder;

    for (std::uint64_t i { 0 }; i < 10; ++i)
        recorder.on_event(event(kernel::clock_tag::audio, i));

    for (std::uint64_t i { 0 }; i < 5; ++i)
        recorder.on_event(event(kernel::clock_tag::ui, 100 + i));

    std::vector<debug::recorded_event> drained;
    const auto moved { recorder.drain(drained, 100) };

    REQUIRE(moved == 15);
    REQUIRE(drained.size() == 15);

    // Per-tag order is preserved even though the drain round-robins.
    std::vector<std::uint64_t> audio_sequences;
    std::vector<std::uint64_t> ui_sequences;

    for (const auto& recorded : drained)
    {
        if (recorded.event.clock == kernel::clock_tag::audio)
            audio_sequences.push_back(recorded.event.sequence);
        else
            ui_sequences.push_back(recorded.event.sequence);
    }

    REQUIRE(audio_sequences.size() == 10);
    REQUIRE(ui_sequences.size() == 5);

    for (std::uint64_t i { 0 }; i < audio_sequences.size(); ++i)
        REQUIRE(audio_sequences[i] == i);

    for (std::uint64_t i { 0 }; i < ui_sequences.size(); ++i)
        REQUIRE(ui_sequences[i] == 100 + i);
}

TEST_CASE("debugger/recorder | A full queue drops events and counts them", "[debugger][recorder]")
{
    debug::trace_recorder recorder { /*capacity_per_thread=*/4 };

    for (std::uint64_t i { 0 }; i < 10; ++i)
        recorder.on_event(event(kernel::clock_tag::audio, i));

    std::vector<debug::recorded_event> drained;
    recorder.drain(drained, 100);

    REQUIRE(drained.size() == 4);

    const auto dropped { recorder.take_dropped() };

    // audio is slot 0; the 6 events that did not fit were counted.
    REQUIRE(dropped[0] == 6);
    REQUIRE(dropped[1] == 0);
    REQUIRE(dropped[2] == 0);

    // take_dropped resets.
    const auto again { recorder.take_dropped() };
    REQUIRE(again[0] == 0);
}

TEST_CASE("debugger/recorder | Disabled capture records nothing", "[debugger][recorder]")
{
    debug::trace_recorder recorder;

    REQUIRE(recorder.enabled());

    recorder.set_enabled(false);
    recorder.on_event(event(kernel::clock_tag::audio, 0));

    std::vector<debug::recorded_event> drained;
    REQUIRE(recorder.drain(drained, 100) == 0);

    recorder.set_enabled(true);
    recorder.on_event(event(kernel::clock_tag::audio, 1));

    REQUIRE(recorder.drain(drained, 100) == 1);
    REQUIRE(drained.front().event.sequence == 1);
}

TEST_CASE("debugger/recorder | Events are stamped at capture time", "[debugger][recorder]")
{
    debug::trace_recorder recorder;

    recorder.on_event(event(kernel::clock_tag::audio, 0));

    std::vector<debug::recorded_event> drained;
    recorder.drain(drained, 100);

    REQUIRE(drained.size() == 1);
    REQUIRE(drained.front().stamp_ns > 0);
}

namespace
{
    using debug_test::tracked;
} // namespace

TEST_CASE("debugger/recorder | A traced struct lives until the consumer releases it", "[debugger][recorder]")
{
    int destroyed { 0 };

    debug::trace_recorder recorder;

    const auto payload { kernel::make_user_value<tracked>(nullptr, &destroyed) };
    REQUIRE(std::holds_alternative<kernel::user_ref>(payload));

    recorder.on_event(kernel::trace_event {
        kernel::trace_event_kind::node_tick_end,
        kernel::clock_tag::audio,
        1,
        0,
        1,
        payload,
        0,
    });

    // The recorder holds its own reference; dropping the test's leaves the
    // traced struct alive.
    kernel::release(payload);
    REQUIRE(destroyed == 0);

    std::vector<debug::recorded_event> drained;
    REQUIRE(recorder.drain(drained, 10) == 1);
    REQUIRE(destroyed == 0);

    // The consumer releases after encoding (the server's drain does this).
    kernel::release(drained.front().event.payload);
    REQUIRE(destroyed == 1);
}

TEST_CASE("debugger/recorder | Dropped and undrained events release their references", "[debugger][recorder]")
{
    int destroyed { 0 };

    const auto payload { kernel::make_user_value<tracked>(nullptr, &destroyed) };

    {
        debug::trace_recorder recorder { 1 }; // capacity one: the second drop

        const kernel::trace_event event {
            kernel::trace_event_kind::node_tick_end,
            kernel::clock_tag::audio,
            1,
            0,
            1,
            payload,
            0,
        };

        recorder.on_event(event); // retained by the queue
        recorder.on_event(event); // queue full: dropped, reference released
        REQUIRE(destroyed == 0);

        // Destruction drains the queue, releasing the first event's reference.
    }

    REQUIRE(destroyed == 0); // the test still holds one

    kernel::release(payload);
    REQUIRE(destroyed == 1);
}
