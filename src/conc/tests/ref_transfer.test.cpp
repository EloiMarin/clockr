#include "clockr/conc/link.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>

namespace
{
    // A ref-counted payload the link's retain/release hooks manage, with
    // liveness bookkeeping so a use-after-free is observable (and a data race
    // under TSan).
    struct counted
    {
        std::atomic<int> refs { 1 };
        std::atomic<bool> alive { true };
    };

    std::atomic<int> g_live { 0 };
    std::atomic<int> g_violations { 0 };

    counted*
    make_counted() noexcept
    {
        ++g_live;
        return new counted;
    }

    // Retain-if-live: the reader's contract. A blind increment would revive a
    // released payload and make its later release drop the count twice.
    bool
    retain_counted(counted* payload) noexcept
    {
        if (payload == nullptr)
            return true;

        int count { payload->refs.load(std::memory_order_relaxed) };

        do
        {
            if (count == 0)
            {
                ++g_violations;
                return false;
            }
        }
        while (!payload->refs.compare_exchange_weak(
            count,
            count + 1,
            std::memory_order_relaxed,
            std::memory_order_relaxed));

        return true;
    }

    void
    release_counted(counted* payload) noexcept
    {
        if (payload == nullptr)
            return;

        if (payload->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            payload->alive.store(false, std::memory_order_release);
            --g_live;
            delete payload;
        }
    }
} // namespace

TEST_CASE("conc/link | Latest slots own their reference until reused", "[conc]")
{
    using namespace clockr::conc;

    const auto retain_hook = [](counted* payload) noexcept { return retain_counted(payload); };
    const auto release_hook = [](counted* payload) noexcept { release_counted(payload); };

    g_live.store(0);
    g_violations.store(0);

    clockr::conc::link<counted*, policy::latest> worm;

    counted* a { make_counted() };
    worm.send(a, epoch_id { 1 }, release_hook); // slot 1 owns the test's reference
    REQUIRE(a->refs.load() == 1);
    REQUIRE(a->alive.load());

    // A receive takes a reference inside the slot's version window and gives
    // it to the caller; dropping it leaves the slot's reference.
    const auto got { worm.receive(epoch_id { 1 }, retain_hook, release_hook) };
    REQUIRE(got.has_value());
    REQUIRE(*got == a);
    REQUIRE(a->refs.load() == 2);

    release_counted(*got);
    REQUIRE(a->refs.load() == 1);

    // Fill the rest of the ring: the slot holding `a` is only reused eight
    // epochs later, so `a` must stay alive across them.
    counted* b { make_counted() };
    worm.send(b, epoch_id { 2 }, release_hook);

    for (epoch_id epoch { 3 }; epoch <= 8; ++epoch)
        worm.send(make_counted(), epoch, release_hook);

    REQUIRE(a->refs.load() == 1);
    REQUIRE(a->alive.load());
    REQUIRE(g_live.load() == 8); // a, b and six fillers

    // Epoch 9 reuses slot 1: the store hands `a` back to the release hook,
    // which drops its last reference (do not touch `a` after that). One new
    // object is created, one displaced: the live count is unchanged.
    const auto before { g_live.load() };
    worm.send(make_counted(), epoch_id { 9 }, release_hook);
    REQUIRE(g_live.load() == before);

    worm.release_all(release_hook);
    REQUIRE(g_live.load() == 0);
    REQUIRE(g_violations.load() == 0);
}


