#include "clockr/conc/double_buffer.hpp" // for the inlineable bound this suite sizes against
#include "clockr/conc/slot.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#ifndef CLOCKR_NO_THREADS
namespace
{
    std::atomic<bool> g_start { false };
} // namespace
#endif

TEST_CASE("conc/slot | Stores and loads the latest value", "[conc]")
{
    clockr::conc::slot<std::int64_t> subject { 0 };

    subject.store(42);

    REQUIRE(subject.load() == 42);
    REQUIRE(subject.load() == 42);
}

TEST_CASE("conc/slot | Round-trips payloads larger than the inlineable bound", "[conc]")
{
    using namespace clockr::conc;

    struct payload
    {
        std::int64_t left;
        double right;
    };

    static_assert(sizeof(payload) == 16);
    static_assert(!inlineable<payload>);

    slot<payload> subject { { 1, 2.5 } };

    subject.store({ 7, 1.25 });

    const auto snapshot { subject.load() };

    REQUIRE(snapshot.left == 7);
    REQUIRE(snapshot.right == 1.25);
}

// Hammer case: needs real threads; compiled out on no-thread targets
// (CLOCKR_NO_THREADS).
#ifndef CLOCKR_NO_THREADS
TEST_CASE("conc/slot | A concurrent reader never observes a torn payload", "[conc]")
{
    using namespace clockr::conc;

    struct payload
    {
        std::int64_t left;
        std::int64_t right;
    };

    slot<payload> subject { { 0, 1 } };

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
                subject.store({ i, i + 1 });
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
                const auto snapshot { subject.load() };

                if (snapshot.right != snapshot.left + 1)
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

TEST_CASE("conc/slot | Moving a slot carries the published value", "[conc]")
{
    using namespace clockr::conc;

    slot<std::int64_t> source { 5 };
    source.store(7);

    slot<std::int64_t> destination { std::move(source) };

    REQUIRE(destination.load() == 7);

    slot<std::int64_t> reassigned { 1 };
    reassigned = std::move(destination);

    REQUIRE(reassigned.load() == 7);
}

TEST_CASE("conc/slot | Slots compose into vectors", "[conc]")
{
    using namespace clockr::conc;

    std::vector<slot<std::int64_t>> slots;
    slots.reserve(2);

    slots.emplace_back(1);
    slots.emplace_back(2);

    REQUIRE(slots[0].load() == 1);
    REQUIRE(slots[1].load() == 2);

    slots.emplace_back(3);

    REQUIRE(slots[0].load() == 1);
    REQUIRE(slots[1].load() == 2);
    REQUIRE(slots[2].load() == 3);
}
