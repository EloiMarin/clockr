#include "clockr/kernel/program.hpp"
#include "clockr/kernel/value.hpp"

#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/release_engine.hpp"

#include "clockr/shell/recipe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace clockr;
using kernel::clock_tag;

namespace
{
    // A pooled (non-trivially-copyable) user struct that counts its live
    // instances. Every test asserts `live == 0` after all recipes, programs
    // and engines are gone: that proves every cell the kernel constructed was
    // destroyed exactly once, i.e. the retain/release discipline balances.
    struct tracked
    {
        static inline std::atomic<int> live { 0 };

        int value { 0 };

        explicit tracked(int v) :
            value { v }
        {
            ++live;
        }

        tracked(const tracked& other) :
            value { other.value }
        {
            ++live;
        }

        tracked(tracked&& other) noexcept :
            value { other.value }
        {
            ++live;
        }

        ~tracked()
        {
            --live;
        }
    };
} // namespace

TEST_CASE("kernel/user_value | A delayed pooled struct balances references", "[kernel][user_value]")
{
    {
        const auto network
        {
            shell::input<tracked, clock_tag::ui>("t")
            >> shell::delay<tracked>(tracked { 1 })
            >> shell::output<tracked, clock_tag::ui>("out")
        };

        kernel::program program { network };
        kernel::release_engine engine;

        program.set<tracked>("t", 2);

        engine.step(program, clock_tag::ui); // seed
        REQUIRE(get<tracked>(program, "out").value == 1);

        engine.step(program, clock_tag::ui); // previous input
        REQUIRE(get<tracked>(program, "out").value == 2);

        program.set<tracked>("t", 3);
        engine.step(program, clock_tag::ui);
        REQUIRE(get<tracked>(program, "out").value == 2);
    }

    REQUIRE(tracked::live.load() == 0);
}

TEST_CASE("kernel/user_value | A user delay seed is materialized at construction", "[kernel][user_value]")
{
    {
        const auto network
        {
            shell::input<tracked, clock_tag::ui>("t")
            >> shell::delay<tracked>(tracked { 1 })
            >> shell::output<tracked, clock_tag::ui>("out")
        };

        kernel::program program { network };

        const auto* const home { program.region(clock_tag::ui).home };

        // The seed is already a pool cell before anything ticks: it is built at
        // program construction, never on the ticking thread.
        const auto seed_bytes { home->pool.snapshot().used_bytes };
        REQUIRE(seed_bytes > 0);

        program.set<tracked>("t", 2);
        const auto held_bytes { home->pool.snapshot().used_bytes };
        REQUIRE(held_bytes > seed_bytes); // the port's own cell

        kernel::release_engine engine;

        engine.step(program, clock_tag::ui); // seed
        REQUIRE(get<tracked>(program, "out").value == 1);
        REQUIRE(home->pool.snapshot().used_bytes == held_bytes);

        engine.step(program, clock_tag::ui); // previous input
        REQUIRE(get<tracked>(program, "out").value == 2);
        REQUIRE(home->pool.snapshot().used_bytes == held_bytes);
    }

    REQUIRE(tracked::live.load() == 0);
}

TEST_CASE("kernel/user_value | A throwing tick releases its values", "[kernel][user_value]")
{
    {
        const auto network
        {
            shell::input<tracked, clock_tag::ui>("t")
            >> shell::lift<tracked, tracked>(
                [](const tracked& in)
                {
                    if (in.value < 0)
                        throw std::runtime_error { "boom" };

                    return tracked { in.value + 1 };
                })
            >> shell::output<tracked, clock_tag::ui>("out")
        };

        kernel::program program { network };
        kernel::release_engine engine;

        program.set<tracked>("t", -1);
        REQUIRE_THROWS_AS(engine.step(program, clock_tag::ui), std::runtime_error);

        program.set<tracked>("t", 7);
        engine.step(program, clock_tag::ui);
        REQUIRE(get<tracked>(program, "out").value == 8);
    }

    REQUIRE(tracked::live.load() == 0);
}

TEST_CASE("kernel/user_value | An event input releases its value when cleared", "[kernel][user_value]")
{
    {
        const auto network
        {
            shell::input<std::optional<tracked>, clock_tag::ui>("e")
            >> shell::lift<std::optional<tracked>, std::int64_t>(
                [](const std::optional<tracked>& in) { return in ? in->value : -1; })
            >> shell::output<std::int64_t, clock_tag::ui>("n")
        };

        kernel::program program { network };
        kernel::release_engine engine;

        engine.step(program, clock_tag::ui);
        REQUIRE(get<std::int64_t>(program, "n") == -1);

        program.set<tracked>("e", 5);
        engine.step(program, clock_tag::ui);
        REQUIRE(get<std::int64_t>(program, "n") == 5);

        // The event was consumed and cleared: its reference is gone, so the
        // cell is retired. Reclamation runs on the region's thread (the
        // engine does it at the start of the next tick; call it explicitly
        // here).
        REQUIRE(tracked::live.load() == 1);
        program.reclaim(clock_tag::ui);
        REQUIRE(tracked::live.load() == 0);

        program.set<tracked>("e", 6);
        engine.step(program, clock_tag::ui);
        REQUIRE(get<std::int64_t>(program, "n") == 6);
    }

    REQUIRE(tracked::live.load() == 0);
}

TEST_CASE("kernel/user_value | A latest crossing carries a pooled struct", "[kernel][user_value]")
{
    {
        const auto network
        {
            shell::at<clock_tag::audio, conc::policy::latest>(
                shell::input<tracked, clock_tag::ui>("t"))
            >> shell::lift<tracked, std::int64_t>([](const tracked& in) { return in.value; })
            >> shell::output<std::int64_t, clock_tag::audio>("n")
        };

        kernel::program program { network };
        kernel::release_engine engine;

        program.set<tracked>("t", 41);
        engine.step(program, clock_tag::ui); // publish
        engine.step(program, clock_tag::audio);

        REQUIRE(get<std::int64_t>(program, "n") == 41);

        program.set<tracked>("t", 42);
        engine.step(program, clock_tag::ui);
        engine.step(program, clock_tag::audio);

        REQUIRE(get<std::int64_t>(program, "n") == 42);
    }

    REQUIRE(tracked::live.load() == 0);
}

TEST_CASE("kernel/user_value | A queued crossing transfers a pooled struct", "[kernel][user_value]")
{
    {
        const auto network
        {
            shell::at<clock_tag::audio, conc::policy::queued>(
                shell::input<std::optional<tracked>, clock_tag::ui>("e"))
            >> shell::lift<std::optional<tracked>, std::int64_t>(
                [](const std::optional<tracked>& in) { return in ? in->value : -1; })
            >> shell::output<std::int64_t, clock_tag::audio>("n")
        };

        kernel::program program { network };
        kernel::release_engine engine;

        program.set<tracked>("e", 11);
        engine.step(program, clock_tag::ui);
        engine.step(program, clock_tag::audio);
        REQUIRE(get<std::int64_t>(program, "n") == 11);

        program.set<tracked>("e", 12);
        engine.step(program, clock_tag::ui);
        // The queue holds the value until the consumer reads it: the cell is
        // still alive here, and released by the consuming tick.
        REQUIRE(tracked::live.load() == 1);
        engine.step(program, clock_tag::audio);
        REQUIRE(get<std::int64_t>(program, "n") == 12);

        // The consuming tick released the last reference; the consumer's home
        // is the producer's region (the cell was allocated there), so reclaim
        // on the producing thread.
        REQUIRE(tracked::live.load() == 1);
        program.reclaim(clock_tag::ui);
        REQUIRE(tracked::live.load() == 0);
    }

    REQUIRE(tracked::live.load() == 0);
}

TEST_CASE("kernel/user_value | Debug and release engines agree on user values", "[kernel][user_value]")
{
    {
        const auto network
        {
            shell::input<tracked, clock_tag::ui>("t")
            >> shell::lift<tracked, std::int64_t>([](const tracked& in) { return in.value * 2; })
            >> shell::output<std::int64_t, clock_tag::ui>("n")
        };

        std::string trace;
        kernel::text_trace_sink sink { trace };

        kernel::release_engine release;
        kernel::debug_engine debug { sink };

        kernel::program release_program { network };
        kernel::program debug_program { network };

        release_program.set<tracked>("t", 21);
        debug_program.set<tracked>("t", 21);

        release.step(release_program, clock_tag::ui);
        debug.step(debug_program, clock_tag::ui);

        REQUIRE(get<std::int64_t>(release_program, "n") == 42);
        REQUIRE(get<std::int64_t>(debug_program, "n") == 42);
    }

    REQUIRE(tracked::live.load() == 0);
}

#ifndef CLOCKR_NO_THREADS
TEST_CASE("kernel/user_value | Concurrent latest ticks stay balanced and race-free", "[kernel][user_value][threads]")
{
    {
        const auto network
        {
            shell::at<clock_tag::audio, conc::policy::latest>(
                shell::input<tracked, clock_tag::ui>("t"))
            >> shell::lift<tracked, std::int64_t>([](const tracked& in) { return in.value; })
            >> shell::output<std::int64_t, clock_tag::audio>("n")
        };

        // A generous pool: while a consumer tick is preempted, the grace rule
        // withholds the cells retired during its read window, so reclamation
        // can lag a long way behind a producer under extreme scheduling
        // pressure. Sizing for that worst case is the application's job (the
        // default pool fits ordinary traffic, not a TSan stress test).
        kernel::program program { network, kernel::program_options { .region_pool_bytes = 4 * 1024 * 1024 } };
        kernel::release_engine ui_engine;
        kernel::release_engine audio_engine;

        // Publish once before the threads start so the consumer never has to
        // read a never-set crossing.
        program.set<tracked>("t", 1);
        ui_engine.step(program, clock_tag::ui);

        constexpr int iterations { 20'000 };

        std::atomic<bool> start { false };

        std::thread producer
        {
            [&]
            {
                while (!start.load(std::memory_order_acquire))
                {}

                for (int i { 2 }; i <= iterations; ++i)
                {
                    // A new cell each tick is retired by the crossing; the
                    // audio region's releases land in the ui home's queue.
                    program.set<tracked>("t", i);
                    ui_engine.step(program, clock_tag::ui);
                }
            }
        };

        std::thread consumer
        {
            [&]
            {
                while (!start.load(std::memory_order_acquire))
                {}

                for (int i { 0 }; i < iterations; ++i)
                    audio_engine.step(program, clock_tag::audio);
            }
        };

        start.store(true, std::memory_order_release);

        producer.join();
        consumer.join();

        audio_engine.step(program, clock_tag::audio);
        REQUIRE(get<std::int64_t>(program, "n") > 1);
    }

    // Every cell the two threads created was reclaimed: no leak, no
    // double-free (the pool emptiness assert in ~program would also fire).
    REQUIRE(tracked::live.load() == 0);
}
#endif
