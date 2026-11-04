#include "clockr/mem/arena.hpp"
#include "clockr/mem/pool.hpp"
#include "clockr/mem/pool_allocator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace
{
    std::uintptr_t
    address_of(const void* ptr);

    bool
    is_aligned(const void* ptr, std::size_t align);
}

TEST_CASE("mem/arena | Allocates an aligned region of the requested size", "[mem]")
{
    using namespace clockr;

    const mem::arena storage { 1024 };

    REQUIRE(storage.data() != nullptr);
    REQUIRE(storage.size() >= 1024);
    REQUIRE(is_aligned(storage.data(), mem::arena::alignment));
}

TEST_CASE("mem/arena | Is move-only", "[mem]")
{
    using namespace clockr;

    static_assert(!std::is_copy_constructible_v<mem::arena>);
    static_assert(std::is_move_constructible_v<mem::arena>);
}

TEST_CASE("mem/pool | Rejects storage smaller than the TLSF minimum", "[mem]")
{
    using namespace clockr;

    REQUIRE_THROWS_AS(mem::pool { mem::arena { mem::pool::minimal_size() - mem::arena::alignment } }, std::bad_alloc);
    REQUIRE_NOTHROW(mem::pool { mem::arena { mem::pool::minimal_size() } });
}

TEST_CASE("mem/pool | Allocate/deallocate round trip", "[mem]")
{
    using namespace clockr;

    mem::pool storage { mem::arena { 64 * 1024 } };

    void* p { storage.allocate(64) };

    REQUIRE(p != nullptr);
    REQUIRE(storage.owns(p));

    const auto used_after_alloc { storage.snapshot().used_bytes };

    storage.deallocate(p);

    REQUIRE(storage.snapshot().used_bytes == used_after_alloc - 64);
    REQUIRE(storage.check());
}

TEST_CASE("mem/pool | Returns blocks aligned to the TLSF alignment", "[mem]")
{
    using namespace clockr;

    mem::pool storage { mem::arena { 64 * 1024 } };

    for (std::size_t i { 0 }; i < 128; ++i)
    {
        void* p { storage.allocate(1) };

        REQUIRE(p != nullptr);

        // The typeless floor is pointer alignment: TLSF granularity follows
        // pointer size (8 on 64-bit hosts, 4 on 32-bit targets like wasm32),
        // and the 64-aligned arena base preserves it down the block chain.
        // Typed over-alignment is pool_allocator's job (alignof(T)).
        REQUIRE(is_aligned(p, alignof(void*)));
    }
}

TEST_CASE("mem/pool | Supports over-aligned allocations", "[mem]")
{
    using namespace clockr;

    mem::pool storage { mem::arena { 64 * 1024 } };

    for (std::size_t align : { std::size_t { 32 }, std::size_t { 64 } })
    {
        void* p { storage.allocate_aligned(100, align) };

        REQUIRE(p != nullptr);
        REQUIRE(is_aligned(p, align));
    }
}

TEST_CASE("mem/pool | Reuses freed memory", "[mem]")
{
    using namespace clockr;

    mem::pool storage { mem::arena { 64 * 1024 } };

    void* p { storage.allocate(128) };
    storage.deallocate(p);

    void* q { storage.allocate(128) };

    REQUIRE(q != nullptr);
    REQUIRE(q == p);
}

TEST_CASE("mem/pool | Exhausts and returns nullptr", "[mem]")
{
    using namespace clockr;

    constexpr std::size_t block_size { 512 };
    mem::pool storage { mem::arena { 64 * 1024 } };

    std::vector<void*> blocks;

    for (std::size_t i { 0 }; ; ++i)
    {
        void* p { storage.allocate(block_size) };

        if (!p)
            break;

        blocks.push_back(p);

        if (i > 10000)
            FAIL("pool never exhausted");
    }

    REQUIRE(blocks.size() > 8);

    for (void* p : blocks)
        REQUIRE(storage.owns(p));

    for (void* p : blocks)
        storage.deallocate(p);

    REQUIRE(storage.check());
}

TEST_CASE("mem/pool | Snapshot accounts for every byte", "[mem]")
{
    using namespace clockr;

    constexpr std::size_t pool_bytes { 64 * 1024 };
    mem::pool storage { mem::arena { pool_bytes } };

    std::vector<void*> blocks;

    for (int i { 0 }; i < 16; ++i)
        blocks.push_back(storage.allocate(256));

    const auto stats { storage.snapshot() };

    REQUIRE(stats.used_bytes == 16 * 256);
    REQUIRE(stats.used_bytes + stats.free_bytes + stats.overhead_bytes == pool_bytes);
}

TEST_CASE("mem/pool | Survives interleaved allocate/free churn", "[mem]")
{
    using namespace clockr;

    constexpr std::size_t pool_bytes { 1 * 1024 * 1024 };
    mem::pool storage { mem::arena { pool_bytes } };

    std::mt19937 rng { 42 };
    std::uniform_int_distribution<std::size_t> size_dist { 1, 512 };
    std::vector<void*> live;

    for (int round { 0 }; round < 8; ++round)
    {
        for (int i { 0 }; i < 256; ++i)
        {
            void* p { storage.allocate(size_dist(rng)) };

            REQUIRE(p != nullptr);

            live.push_back(p);
        }

        for (int i { 0 }; i < 128; ++i)
        {
            storage.deallocate(live.back());
            live.pop_back();
        }
    }

    for (void* p : live)
        storage.deallocate(p);

    REQUIRE(storage.check());
}

TEST_CASE("mem/pool_allocator | Keeps every allocation inside the pool", "[mem]")
{
    using namespace clockr;

    mem::pool storage { mem::arena { 128 * 1024 } };

    std::vector<std::int64_t, mem::pool_allocator<std::int64_t>> values { mem::pool_allocator<std::int64_t> { storage } };

    for (std::size_t i { 0 }; i < 4096; ++i)
        values.push_back(static_cast<std::int64_t>(i));

    REQUIRE(values.size() == 4096);

    for (std::size_t i { 0 }; i < 4096; ++i)
        REQUIRE(values[i] == static_cast<std::int64_t>(i));

    for (std::size_t i { 0 }; i < 4096; ++i)
        REQUIRE(storage.owns(&values[i]));

    // Typed alignment holds even where the raw floor does not: int64_t needs
    // 8 on every target, including 32-bit ones with a 4-byte pool floor.
    REQUIRE(is_aligned(values.data(), alignof(std::int64_t)));

    REQUIRE(storage.check());
}

// libc++/libstdc++ only instantiate the rebinding constructor lazily (unlike
// MSVC's _Get_proxy_allocator), so name it explicitly: a rebind must carry the
// source allocator's pool across, not default to a fresh one.
TEST_CASE("mem/pool_allocator | Rebound allocators share the source pool", "[mem]")
{
    using namespace clockr;

    mem::pool storage { mem::arena { 128 * 1024 } };

    mem::pool_allocator<std::int64_t> source { storage };
    mem::pool_allocator<double> rebound { source };

    REQUIRE(source == rebound);

    double* cells { rebound.allocate(2) };
    REQUIRE(storage.owns(cells));
    rebound.deallocate(cells, 2);

    REQUIRE(storage.check());
}

namespace
{
    std::uintptr_t
    address_of(const void* ptr)
    {
        return reinterpret_cast<std::uintptr_t>(ptr);
    }

    bool
    is_aligned(const void* ptr, std::size_t align)
    {
        return address_of(ptr) % align == 0;
    }
}
