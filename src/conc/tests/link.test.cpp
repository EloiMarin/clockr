#include "clockr/conc/double_buffer.hpp"
#include "clockr/conc/spsc.hpp"
#include "clockr/conc/link.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

#ifndef CLOCKR_NO_THREADS
namespace
{
    std::atomic<bool> g_start { false };
} // namespace
#endif

TEST_CASE("conc/double_buffer | Stores and loads the latest value", "[conc]")
{
    using namespace clockr::conc;

    double_buffer<std::int64_t> buffer { 0 };

    buffer.store(42);

    REQUIRE(buffer.load() == 42);
    REQUIRE(buffer.load() == 42);
}

TEST_CASE("conc/double_buffer | Accepts inlineable payloads", "[conc]")
{
    struct payload
    {
        std::int32_t left;
        std::int32_t right;
    };

    static_assert(clockr::conc::inlineable<bool>);
    static_assert(clockr::conc::inlineable<double>);
    static_assert(clockr::conc::inlineable<std::int64_t>);
    static_assert(clockr::conc::inlineable<payload>);
    static_assert(!clockr::conc::inlineable<std::string>);

    clockr::conc::double_buffer<payload> buffer { { 0, 0 } };

    buffer.store({ 2, 3 });

    REQUIRE(buffer.load().left == 2);
    REQUIRE(buffer.load().right == 3);
}

// Hammer cases need real threads; they are compiled out on no-thread targets
// (CLOCKR_NO_THREADS) while the single-threaded cases in this file run.
#ifndef CLOCKR_NO_THREADS
TEST_CASE("conc/double_buffer | Hands off values across threads without tearing", "[conc]")
{
    using namespace clockr::conc;

    struct payload
    {
        std::int32_t left;
        std::int32_t right;
    };

    double_buffer<payload> buffer { { 0, 1 } };

    constexpr std::int64_t rounds { 1'000'000 };

    std::atomic<std::int64_t> torn { 0 };
    std::atomic<std::int64_t> last { -1 };

    std::thread producer
    {
        [&]
        {
            while (!g_start.load())
                ;

            for (std::int64_t i { 0 }; i < rounds; ++i)
                buffer.store({ static_cast<std::int32_t>(i), static_cast<std::int32_t>(i) + 1 });
        }
    };

    std::thread consumer
    {
        [&]
        {
            std::int64_t local_torn { 0 };

            while (!g_start.load())
                ;

            for (;;)
            {
                const auto snapshot { buffer.load() };

                if (snapshot.left + 1 != snapshot.right)
                    ++local_torn;

                last.store(snapshot.left);

                if (snapshot.left == rounds - 1)
                    break;
            }

            torn.store(local_torn);
        }
    };

    g_start.store(true);

    producer.join();
    consumer.join();

    REQUIRE(torn.load() == 0);
    REQUIRE(last.load() == rounds - 1);
}
#endif // CLOCKR_NO_THREADS

TEST_CASE("conc/spsc | Round trips values in order", "[conc]")
{
    using namespace clockr::conc;

    spsc<std::int64_t> queue { 1024 };

    for (std::int64_t i { 0 }; i < 100; ++i)
        REQUIRE(queue.send(i));

    REQUIRE(queue.size() == 100);

    std::int64_t value { 0 };

    for (std::int64_t i { 0 }; i < 100; ++i)
    {
        REQUIRE(queue.receive(value));
        REQUIRE(value == i);
    }

    REQUIRE(!queue.receive(value));
}

TEST_CASE("conc/spsc | Rejects sends once full", "[conc]")
{
    using namespace clockr::conc;

    spsc<std::int64_t> queue { 8 };

    for (std::int64_t i { 0 }; i < 8; ++i)
        REQUIRE(queue.send(i));

    REQUIRE(!queue.send(9));
}

#ifndef CLOCKR_NO_THREADS
TEST_CASE("conc/spsc | Streams a full workload across threads", "[conc]")
{
    using namespace clockr::conc;

    constexpr std::int64_t count { 100'000 };

    spsc<std::int64_t> queue { 4096 };

    std::atomic<std::int64_t> mismatches { 0 };
    std::atomic<std::int64_t> received { 0 };

    std::thread producer
    {
        [&]
        {
            while (!g_start.load())
                ;

            std::int64_t sent { 0 };

            while (sent < count)
            {
                if (queue.send(sent))
                    ++sent;
            }
        }
    };

    std::thread consumer
    {
        [&]
        {
            std::int64_t value { 0 };
            std::int64_t expected { 0 };

            while (!g_start.load())
                ;

            while (received.load() < count)
            {
                if (queue.receive(value))
                {
                    if (value != expected)
                        ++mismatches;

                    ++expected;
                    ++received;
                }
            }
        }
    };

    g_start.store(true);

    producer.join();
    consumer.join();

    REQUIRE(received.load() == count);
    REQUIRE(mismatches.load() == 0);
}
#endif // CLOCKR_NO_THREADS

TEST_CASE("conc/link | Latest link exchanges the freshest snapshot", "[conc]")
{
    using namespace clockr::conc;

    clockr::conc::link<std::int64_t, policy::latest> hole { 0 };

    // Nothing published yet: initial holds.
    REQUIRE(hole.receive(0) == 0);

    // Same-epoch repeats coalesce (last wins, L9a).
    hole.send(7, 1);
    hole.send(8, 1);

    REQUIRE(hole.receive(1) == 8);

    // Silent epochs hold the last defined value.
    REQUIRE(hole.receive(5) == 8);

    hole.send(9, 6);

    REQUIRE(hole.receive(6) == 9);
    // Stale cut still finds its own epoch while history retains it.
    REQUIRE(hole.receive(1) == 8);
    // A snapshot lapped by more than the ring reports stale (retry with fresh).
    for (epoch_id e { 7 }; e < 7 + 16; ++e)
        hole.send(static_cast<std::int64_t>(e), e);
    REQUIRE(!hole.receive(1).has_value());
    REQUIRE(hole.receive(22) == 22);
}

TEST_CASE("conc/link | Queued link delivers every message in order", "[conc]")
{
    using namespace clockr::conc;

    clockr::conc::link<std::int64_t, policy::queued> hole { 64 };

    for (std::int64_t i { 0 }; i < 32; ++i)
        REQUIRE(hole.send(i, static_cast<epoch_id>(i + 1)));

    std::int64_t value { 0 };

    for (std::int64_t i { 0 }; i < 32; ++i)
    {
        REQUIRE(hole.receive(value, 32));
        REQUIRE(value == i);
    }

    REQUIRE(!hole.receive(value, 32));
}

TEST_CASE("conc/link | Queued link hides future epochs until published", "[conc]")
{
    using namespace clockr::conc;

    clockr::conc::link<std::int64_t, policy::queued> hole { 64 };

    REQUIRE(hole.send(1, 1));
    REQUIRE(hole.send(2, 2));

    std::int64_t value { 0 };

    // Epoch 2 sent but only epoch 1 published: only epoch 1 is visible.
    REQUIRE(hole.receive(value, 1));
    REQUIRE(value == 1);
    REQUIRE(!hole.receive(value, 1));

    // Publishing epoch 2 releases it.
    REQUIRE(hole.receive(value, 2));
    REQUIRE(value == 2);
    REQUIRE(!hole.receive(value, 2));
}

TEST_CASE("conc/link | Latest mode carries payloads beyond the inlineable bound", "[conc]")
{
    using namespace clockr::conc;

    struct payload
    {
        std::int64_t left;
        double right;
    };

    static_assert(!inlineable<payload>);

    clockr::conc::link<payload, policy::latest> hole { { 0, 0.0 } };

    hole.send({ 7, 1.5 }, 1);

    const auto snapshot { hole.receive(1) };

    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->left == 7);
    REQUIRE(snapshot->right == 1.5);
}

#ifndef CLOCKR_NO_THREADS
TEST_CASE("conc/link | Latest link transfers an uninterrupted stream of small values", "[conc]")
{
    using namespace clockr::conc;

    constexpr std::int64_t rounds { 1'000'000 };

    clockr::conc::link<std::int64_t, policy::latest> hole { 0 };
    std::atomic<epoch_id> published { 0 };

    std::atomic<std::int64_t> last { -1 };

    std::thread producer
    {
        [&]
        {
            while (!g_start.load())
                ;

            for (std::int64_t i { 0 }; i < rounds; ++i)
            {
                const auto epoch { static_cast<epoch_id>(i + 1) };
                hole.send(i, epoch);
                published.store(epoch, std::memory_order_release);
            }
        }
    };

    std::thread consumer
    {
        [&]
        {
            std::int64_t local_last { -1 };

            while (!g_start.load())
                ;

            while (local_last < rounds - 1)
            {
                const auto cut { published.load(std::memory_order_acquire) };

                if (const auto got { hole.receive(cut) })
                    local_last = *got;
                // else: snapshot lapped by the producer's lead — reload fresh
                // and retry (same discipline as engine fill).
            }

            last.store(local_last);
        }
    };

    g_start.store(true);

    producer.join();
    consumer.join();

    REQUIRE(last.load() == rounds - 1);
}
#endif // CLOCKR_NO_THREADS
