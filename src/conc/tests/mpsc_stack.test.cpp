#include "clockr/conc/mpsc_stack.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#ifndef CLOCKR_NO_THREADS
namespace
{
    std::atomic<bool> g_start { false };
} // namespace
#endif

namespace
{
    struct node
    {
        int value { 0 };
        std::atomic<node*> next { nullptr };
    };
} // namespace

TEST_CASE("conc/mpsc_stack | Pops nodes in reverse push order", "[conc]")
{
    using namespace clockr::conc;

    mpsc_stack<node> stack;

    REQUIRE(stack.empty());
    REQUIRE(stack.pop() == nullptr);

    node a { 1 };
    node b { 2 };
    node c { 3 };

    stack.push(&a);
    stack.push(&b);
    stack.push(&c);

    REQUIRE_FALSE(stack.empty());
    REQUIRE(stack.pop() == &c);
    REQUIRE(stack.pop() == &b);
    REQUIRE(stack.pop() == &a);
    REQUIRE(stack.pop() == nullptr);
    REQUIRE(stack.empty());
}

#ifndef CLOCKR_NO_THREADS
TEST_CASE("conc/mpsc_stack | Many producers lose no nodes", "[conc]")
{
    using namespace clockr::conc;

    constexpr int producers { 4 };
    constexpr int per_producer { 20'000 };

    // The nodes hold atomics (the intrusive link), so they are not movable;
    // allocate them in place.
    std::vector<std::unique_ptr<node[]>> nodes;
    nodes.reserve(producers);

    for (int p { 0 }; p < producers; ++p)
        nodes.emplace_back(std::make_unique<node[]>(per_producer));

    mpsc_stack<node> stack;
    g_start.store(false, std::memory_order_relaxed);

    std::vector<std::thread> threads;
    threads.reserve(producers);

    for (int p { 0 }; p < producers; ++p)
    {
        threads.emplace_back(
            [&, p]
            {
                while (!g_start.load(std::memory_order_acquire))
                {}

                for (int i { 0 }; i < per_producer; ++i)
                    stack.push(&nodes[static_cast<std::size_t>(p)][static_cast<std::size_t>(i)]);
            });
    }

    g_start.store(true, std::memory_order_release);

    std::size_t popped { 0 };

    // The consumer drains until every producer is done and the stack is empty.
    while (popped < static_cast<std::size_t>(producers * per_producer))
    {
        if (stack.pop() != nullptr)
            ++popped;
    }

    for (auto& thread : threads)
        thread.join();

    REQUIRE(popped == static_cast<std::size_t>(producers * per_producer));
    REQUIRE(stack.empty());
}
#endif
