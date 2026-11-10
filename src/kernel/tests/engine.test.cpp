#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/kernel/trace.hpp"

#include "clockr/kernel/graph.hpp"

#include "clockr/conc/link.hpp"

#include "clockr/shell/recipe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace Catch
{
    template<>
    struct StringMaker<clockr::kernel::value>
    {
        static std::string
        convert(clockr::kernel::value const& p)
        {
            return std::visit([](auto const& value) -> std::string
            {
                using T = std::decay_t<decltype(value)>;

                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    return "unit";
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    return "bool<" + std::string{ value ? "true" : "false" } + ">";
                }
                else if constexpr (std::is_same_v<T, std::int64_t>)
                {
                    return "int64_t<" + std::to_string(value) + ">";
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    return "double<" + std::to_string(value) + ">";
                }
                else
                {
                    // User-typed alternatives. Written as an `else` (not a
                    // trailing return) so MSVC does not report C4702
                    // "unreachable code" for the scalar instantiations where
                    // an earlier branch always returns.
                    return "(unknown)";
                }
            }, p);
        }
    };
}

namespace
{
    using namespace clockr;

    // Two-input summing network (replaces the hand-built kernel graph: the
    // shell is the only way to build graphs now).
    auto
    build_sum_network()
    {
        return shell::zip(
                   shell::input<std::int64_t, kernel::clock_tag::audio>("a"),
                   shell::input<std::int64_t, kernel::clock_tag::audio>("b"))
            >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
                   [](const std::int64_t& x, const std::int64_t& y) { return x + y; })
            >> shell::output<std::int64_t, kernel::clock_tag::audio>("out");
    }

    kernel::value
    evaluate_once(
        kernel::engine& e,
        const kernel::descriptor_view& g,
        kernel::value in,
        std::span<kernel::delay_state> state
    );

    // 24 bytes: above value.hpp's user_inline_capacity (16), so a value of
    // this type travels through a ref-counted pool cell, not inline bytes.
    struct pooled
    {
        std::int64_t a { 0 };
        std::int64_t b { 0 };
        std::int64_t c { 0 };
    };

    // Attribute needed: a body that only throws is otherwise flagged by
    // -Wmissing-noreturn under -Weverything.
    [[noreturn]] std::int64_t
    throwing_tick(const std::int64_t&)
    {
        throw 42;
    }

    std::size_t g_allocations { 0 };
    bool g_counting { false };
} // namespace

// The allocation tests below count through a replacement global `operator new`.
// ThreadSanitizer's runtime ships its own replacement in libclang_rt.tsan_cxx,
// so defining ours collides at link time ("multiple definition of operator
// new"). Under TSan the hook is therefore unavailable, and the tests that need
// it are skipped; they still run (and pin the no-heap-allocate requirement) in
// every other configuration.
#if defined(__SANITIZE_THREAD__)
#  define CLOCKR_TEST_ALLOC_HOOK 0
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CLOCKR_TEST_ALLOC_HOOK 0
#  endif
#endif

#ifndef CLOCKR_TEST_ALLOC_HOOK
#  define CLOCKR_TEST_ALLOC_HOOK 1
#endif

#if CLOCKR_TEST_ALLOC_HOOK
namespace
{
// MSVC has no std::aligned_alloc; its aligned allocation family is
// _aligned_malloc/_aligned_free and the two must be paired (a block from one
// cannot be released by the other). The standard aligned new/delete overloads
// below are the only users, so pairing them here is enough.
void*
aligned_alloc_bytes(std::size_t bytes, std::size_t alignment) noexcept
{
#if defined(_MSC_VER)
    return _aligned_malloc(bytes, alignment);
#else
    return std::aligned_alloc(alignment, bytes);
#endif
}

void
aligned_free_bytes(void* p) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace

void*
operator new(std::size_t n)
{
    if (g_counting)
        ++g_allocations;

    return std::malloc(n);
}

void*
operator new[](std::size_t n)
{
    if (g_counting)
        ++g_allocations;

    return std::malloc(n);
}

// Over-aligned allocations count too: user-cell storage can request more than
// the default alignment, and an uncounted overload would hide that traffic.
void*
operator new(std::size_t n, std::align_val_t align)
{
    if (g_counting)
        ++g_allocations;

    const auto a { static_cast<std::size_t>(align) };
    const auto rounded { ((n + a - 1) / a) * a };

    return aligned_alloc_bytes(rounded, a);
}

void*
operator new[](std::size_t n, std::align_val_t align)
{
    if (g_counting)
        ++g_allocations;

    const auto a { static_cast<std::size_t>(align) };
    const auto rounded { ((n + a - 1) / a) * a };

    return aligned_alloc_bytes(rounded, a);
}

void
operator delete(void* p) noexcept
{
    std::free(p);
}

void
operator delete[](void* p) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

void
operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::align_val_t) noexcept
{
    aligned_free_bytes(p);
}

void
operator delete[](void* p, std::align_val_t) noexcept
{
    aligned_free_bytes(p);
}

void
operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    aligned_free_bytes(p);
}

void
operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    aligned_free_bytes(p);
}
#endif // CLOCKR_TEST_ALLOC_HOOK

TEST_CASE("kernel | Release engine evaluates inputs through the graph", "[kernel]")
{
    const auto network { build_sum_network() };

    kernel::release_engine engine;
    kernel::program program { network };

    // Set up the program with inputs
    program.set("a", 2);
    program.set("b", 3);

    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(kernel::get<std::int64_t>(program, "out") == 5);
}

TEST_CASE("kernel | Debug engine produces the same results as the release engine", "[kernel]")
{
    const auto network { build_sum_network() };

    std::string trace;
    kernel::text_trace_sink sink { trace };

    kernel::release_engine release;
    kernel::debug_engine debug { sink };

    std::vector<kernel::value> inputs { kernel::value { std::int64_t { 7 } }, kernel::value { std::int64_t { 8 } } };
    std::vector<kernel::value> release_outputs(1);
    std::vector<kernel::value> debug_outputs(1);
    std::vector<kernel::delay_state> state;

    kernel::program release_program { network };
    kernel::program debug_program { network };
    
    // Set up the programs with inputs
    release_program.set(0, inputs[0]);
    release_program.set(1, inputs[1]);
    debug_program.set(0, inputs[0]);
    debug_program.set(1, inputs[1]);

    release.step(release_program, kernel::clock_tag::audio);
    debug.step(debug_program, kernel::clock_tag::audio);

    REQUIRE(release_program.get(0) == debug_program.get(0));
    REQUIRE(std::get<std::int64_t>(debug_program.get(0)) == 15);
}

TEST_CASE("kernel | Debug engine traces every evaluation step in order", "[kernel]")
{
    const auto network { build_sum_network() };

    std::string trace;
    kernel::text_trace_sink sink { trace };

    kernel::debug_engine engine { sink };

    std::vector<kernel::value> inputs { kernel::value { std::int64_t { 2 } }, kernel::value { std::int64_t { 3 } } };

    kernel::program program { network };
    program.set(0, inputs[0]);
    program.set(1, inputs[1]);

    engine.step(program, kernel::clock_tag::audio);

    // One message per node evaluation: the begin/end pair collapsed on the
    // transport, so no node_tick_begin lines remain.
    std::size_t begin_count { 0 };
    std::size_t end_count { 0 };
    std::size_t region_begin_count { 0 };
    std::size_t region_end_count { 0 };
    std::size_t region_begin_pos { std::string::npos };
    std::size_t region_end_pos { 0 };

    for (std::size_t pos { 0 }; (pos = trace.find("\"kind\":\"node_tick_begin\"", pos)) != std::string::npos; ++pos)
        ++begin_count;

    for (std::size_t pos { 0 }; (pos = trace.find("\"kind\":\"node_tick_end\"", pos)) != std::string::npos; ++pos)
        ++end_count;

    for (std::size_t pos { 0 }; (pos = trace.find("\"kind\":\"region_step_begin\"", pos)) != std::string::npos; ++pos)
    {
        ++region_begin_count;
        region_begin_pos = region_begin_pos == std::string::npos ? pos : region_begin_pos;
    }

    for (std::size_t pos { 0 }; (pos = trace.find("\"kind\":\"region_step_end\"", pos)) != std::string::npos; ++pos)
    {
        ++region_end_count;
        region_end_pos = pos;
    }

    REQUIRE(begin_count == 0);
    REQUIRE(end_count == 4);
    REQUIRE(region_begin_count == 1);
    REQUIRE(region_end_count == 1);

    REQUIRE(region_begin_pos < region_end_pos);
}

namespace
{
    // Records the region brackets a tick produces, so tests can pin the
    // tick id (epoch) and the threw flag without parsing text.
    class region_probe_engine final : public kernel::engine
    {
    public:
        struct bracket
        {
            kernel::clock_tag clock;
            conc::epoch_id epoch;
            bool threw;
        };

        std::vector<bracket> began;
        std::vector<bracket> ended;

    protected:
        void
        on_region_step_begin(kernel::clock_tag clock, conc::epoch_id epoch) override
        {
            began.push_back({ clock, epoch, false });
        }

        void
        on_region_step_end(kernel::clock_tag clock, conc::epoch_id epoch, bool threw) override
        {
            ended.push_back({ clock, epoch, threw });
        }
    };
} // namespace

TEST_CASE("kernel | Region hooks fire once per tick with the region epoch", "[kernel]")
{
    const auto network { build_sum_network() };

    region_probe_engine engine;
    kernel::program program { network };

    program.set("a", 1);
    program.set("b", 2);

    engine.step(program, kernel::clock_tag::audio);
    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(engine.began.size() == 2);
    REQUIRE(engine.ended.size() == 2);

    // Epochs are the region published_epoch counter: first tick publishes 1.
    REQUIRE(engine.began[0].clock == kernel::clock_tag::audio);
    REQUIRE(engine.began[0].epoch == 1);
    REQUIRE(engine.began[1].epoch == 2);

    REQUIRE(engine.ended[0].clock == kernel::clock_tag::audio);
    REQUIRE(engine.ended[0].epoch == 1);
    REQUIRE(engine.ended[0].threw == false);
    REQUIRE(engine.ended[1].epoch == 2);
}

TEST_CASE("kernel | Debug engine inlines small values into the trace", "[kernel]")
{
    // Same 3-node shape as the hand-built graph this replaces (input 0,
    // threshold lift 1, output 2), so the pinned node ids and sequence
    // numbers below are unchanged.
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
            >> shell::lift<std::int64_t, bool>([](const std::int64_t& v) { return v > 10; })
            >> shell::output<bool, kernel::clock_tag::ui>("out")
    };

    std::string trace;
    kernel::text_trace_sink sink { trace };

    kernel::debug_engine engine { sink };

    std::vector<kernel::value> inputs { kernel::value { std::int64_t { 42 } } };

    kernel::program program { network };
    program.set(0, inputs[0]);
    
    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(std::get<bool>(program.get(0)));
    // Durations are timing-dependent, so each line is pinned only up to the
    // duration_ns field (whose presence is the assertion).
    REQUIRE(trace.find("{\"kind\":\"node_tick_end\",\"clock\":\"ui\",\"epoch\":1,\"node\":0,\"seq\":1,\"value\":42,\"duration_ns\":") != std::string::npos);
    REQUIRE(trace.find("{\"kind\":\"node_tick_end\",\"clock\":\"ui\",\"epoch\":1,\"node\":1,\"seq\":2,\"value\":true,\"duration_ns\":") != std::string::npos);
    REQUIRE(trace.find("{\"kind\":\"node_tick_end\",\"clock\":\"ui\",\"epoch\":1,\"node\":2,\"seq\":3,\"value\":true,\"duration_ns\":") != std::string::npos);
    REQUIRE(trace.find("{\"kind\":\"region_step_begin\",\"clock\":\"ui\",\"epoch\":1,\"node\":0,\"seq\":0}") != std::string::npos);
    REQUIRE(trace.find("{\"kind\":\"region_step_end\",\"clock\":\"ui\",\"epoch\":1,\"node\":0,\"seq\":4,\"threw\":false,\"duration_ns\":") != std::string::npos);
}

TEST_CASE("kernel | Debug engine records which node choked and on what", "[kernel]")
{
    // Same 3-node shape as above (input 0, lift 1, output 2), but the input
    // is never set: the lift's strict from_value<double> throws on the
    // monostate storage. The throw still propagates (release parity below),
    // but debug keeps who and what: node 1, fan-in [monostate]. That record
    // is the E1 totality check in miniature — a plain-T lift observing
    // monostate is always a bug (missing absorber or missing startup
    // publish), and now it names names instead of just unwinding.
    const auto network
    {
        shell::input<double, kernel::clock_tag::ui>("in")
            >> shell::lift<double, double>([](const double& v) { return v * 2.0; })
            >> shell::output<double, kernel::clock_tag::ui>("out")
    };

    std::string trace;
    kernel::text_trace_sink sink { trace };

    kernel::debug_engine debug { sink };
    kernel::release_engine release;

    kernel::program debug_program { network };
    kernel::program release_program { network };

    REQUIRE_THROWS_AS(debug.step(debug_program, kernel::clock_tag::ui), std::bad_variant_access);
    REQUIRE_THROWS_AS(release.step(release_program, kernel::clock_tag::ui), std::bad_variant_access);

    REQUIRE(debug.has_failure());
    REQUIRE(debug.failure().node == 1);
    REQUIRE(debug.failure().fan_in.size() == 1);
    REQUIRE(std::holds_alternative<std::monostate>(debug.failure().fan_in.front()));

    // The trace closes the region bracket on the unwind path, marked threw.
    // The throw carries the failed evaluation's measured time, like an end.
    REQUIRE(trace.find("{\"kind\":\"node_tick_throw\",\"clock\":\"ui\",\"epoch\":1,\"node\":1,\"seq\":2,\"duration_ns\":") != std::string::npos);
    REQUIRE(trace.find("{\"kind\":\"region_step_end\",\"clock\":\"ui\",\"epoch\":1,\"node\":0,\"seq\":3,\"threw\":true,\"duration_ns\":") != std::string::npos);

    // The structured hooks see the same bracket without parsing text: one
    // begin, one end, closed as threw on epoch 1.
    {
        region_probe_engine probe;
        kernel::program probe_program { network };

        REQUIRE_THROWS_AS(probe.step(probe_program, kernel::clock_tag::ui), std::bad_variant_access);

        REQUIRE(probe.began.size() == 1);
        REQUIRE(probe.ended.size() == 1);
        REQUIRE(probe.ended[0].clock == kernel::clock_tag::ui);
        REQUIRE(probe.ended[0].epoch == 1);
        REQUIRE(probe.ended[0].threw);
    }
}

TEST_CASE("kernel | Trace helpers render inlineable values", "[kernel]")
{
    using namespace clockr::kernel;

    REQUIRE(!is_present(kernel::value { std::monostate {} }));

    REQUIRE(is_present(kernel::value { true }));
    REQUIRE(value_text(kernel::value { true }) == "true");
    REQUIRE(value_text(kernel::value { false }) == "false");
    REQUIRE(value_text(kernel::value { std::int64_t { -42 } }) == "-42");
    REQUIRE(value_text(kernel::value { 2.5 }) == "2.500000");
    REQUIRE(value_text(kernel::value { -0.123456789 }) == "-0.123457");
}

TEST_CASE("kernel | A delay outputs its initial value before the first input arrives", "[kernel]")
{ // The §3.1 equation's t = 0 half, directly on the kernel primitive: a
  // hand-written descriptor literal, the one place the bare delay node is
  // covered without the shell (the shift half lives in shell's delay case).
    static constexpr auto passthrough = [](const void*, const kernel::tick_context&, std::span<const kernel::value> inputs)
    {
        return inputs.empty() ? kernel::value { std::monostate {} } : inputs.front();
    };
    static constexpr auto initial = [](const void*, const kernel::tick_context&, std::span<const kernel::value>)
    {
        return kernel::value { std::int64_t { 0 } };
    };

    static constexpr std::array<const kernel::node_descriptor, 3> nodes
    {
        kernel::node_descriptor{ 0, kernel::clock_tag::ui, kernel::node_kind::input, passthrough, nullptr },
        kernel::node_descriptor{ 1, kernel::clock_tag::ui, kernel::node_kind::delay, initial, nullptr },
        kernel::node_descriptor{ 2, kernel::clock_tag::ui, kernel::node_kind::output, passthrough, nullptr },
    };
    static constexpr std::array<const kernel::edge, 2> edges
    {
        kernel::edge{ 0, 1 },
        kernel::edge{ 1, 2 },
    };
    static constexpr std::array<const kernel::node_id, 2> fan_in { 0, 1 };
    static constexpr std::array<const std::size_t, 4> fan_in_offsets { 0, 0, 1, 2 };
    static constexpr std::array<const kernel::node_id, 3> order { 0, 1, 2 };
    static constexpr std::array<const kernel::region, 0> regions;
    static constexpr std::array<const kernel::crossing, 0> crossings;
    static constexpr std::array<const kernel::node_id, 1> input_ids { 0 };
    static constexpr std::array<const kernel::node_id, 1> output_ids { 2 };
    static constexpr std::array<const char*, 1> input_names { "in" };
    static constexpr std::array<const char*, 1> output_names { "out" };
    static constexpr std::array<const kernel::node_label, 0> labels;
    static constexpr kernel::descriptor_view descriptor
    {
        nodes, labels, edges, fan_in, fan_in_offsets, order, regions, crossings,
        input_ids, output_ids, input_names, output_names,
    };

    kernel::release_engine engine;

    std::vector<kernel::delay_state> state(1);

    REQUIRE(evaluate_once(engine, descriptor, 10, state) == kernel::value { 0 });
}

TEST_CASE("kernel | Feedback through a delay is a counter", "[A1][kernel]")
{ // The §3.1 equation, used with feedback: d → inc → d is legal (L7), and
  // each tick() call is one tick, so state evolves across calls.
  // Tick t outputs 23 + t. The unit input is never set: it defaults to
  // monostate, which the step ignores.
    const auto network
    {
        shell::input<std::monostate, kernel::clock_tag::ui>("tick")
            >> shell::scan<std::monostate, std::int64_t>(
                   23,
                   [](const std::int64_t& previous, const std::monostate&) { return previous + 1; })
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::release_engine engine;
    kernel::program program { network };

    for (std::int64_t tick { 1 }; tick <= 3; ++tick)
    {
        engine.step(program, kernel::clock_tag::ui);
        REQUIRE(program.get(0) == kernel::value { 23 + tick });
    }
}

TEST_CASE("kernel | Engines agree on stateful graphs", "[A1][kernel]")
{
    // L5 extended to state: debug and release must produce identical output
    // sequences across ticks (state is data, so parity is checkable at all).
    const auto network
    {
        shell::input<std::monostate, kernel::clock_tag::ui>("tick")
            >> shell::scan<std::monostate, std::int64_t>(
                   0,
                   [](const std::int64_t& previous, const std::monostate&) { return previous + 1; })
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    std::string trace;
    kernel::text_trace_sink sink { trace };

    kernel::release_engine release;
    kernel::debug_engine debug { sink };

    kernel::program release_program { network };
    kernel::program debug_program { network };

    for (int tick { 0 }; tick < 3; ++tick)
    {
        release.step(release_program, kernel::clock_tag::ui);
        debug.step(debug_program, kernel::clock_tag::ui);

        REQUIRE(release_program.get(0) == debug_program.get(0));
    }
}

TEST_CASE("kernel | Evaluate performs no heap allocations", "[kernel]")
{
#if !CLOCKR_TEST_ALLOC_HOOK
    SKIP("allocation counting is unavailable under ThreadSanitizer");
#endif

    const auto network { build_sum_network() };

    kernel::program program { network };

    program.set("a", 6);
    program.set("b", 7);

    kernel::release_engine engine;

    g_counting = true;
    g_allocations = 0;

    engine.step(program, kernel::clock_tag::audio);

    g_counting = false;

    REQUIRE(g_allocations == 0);
    REQUIRE(kernel::get<std::int64_t>(program, "out") == 13);
}

TEST_CASE("kernel | Evaluate performs no heap allocations across regions", "[kernel]")
{
#if !CLOCKR_TEST_ALLOC_HOOK
    SKIP("allocation counting is unavailable under ThreadSanitizer");
#endif

    // Both crossing policies in one program: latest (state) and queued
    // (events). Scratch buffers and transferred cells come from the region
    // pool, so a full ui+audio round must not touch the system heap.
    const auto network
    {
        shell::zip(
            shell::at<kernel::clock_tag::audio, conc::policy::latest>(
                shell::input<std::int64_t, kernel::clock_tag::ui>("n")),
            shell::at<kernel::clock_tag::audio, conc::policy::queued>(
                shell::input<std::int64_t, kernel::clock_tag::ui>("m")))
        >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
               [](const std::int64_t& x, const std::int64_t& y) { return x + y; })
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::program program { network };
    kernel::release_engine engine;

    // Warm-up round: the first cross fills the crossing rings.
    program.set("n", std::int64_t { 1 });
    program.set("m", std::int64_t { 2 });
    engine.step(program, kernel::clock_tag::ui);
    engine.step(program, kernel::clock_tag::audio);

    program.set("n", std::int64_t { 3 });
    program.set("m", std::int64_t { 4 });

    g_counting = true;
    g_allocations = 0;

    engine.step(program, kernel::clock_tag::ui);
    engine.step(program, kernel::clock_tag::audio);

    g_counting = false;

    REQUIRE(g_allocations == 0);
    REQUIRE(kernel::get<std::int64_t>(program, "out") == 7);
}

TEST_CASE("kernel | Evaluate performs no heap allocations for pooled user values", "[kernel]")
{
#if !CLOCKR_TEST_ALLOC_HOOK
    SKIP("allocation counting is unavailable under ThreadSanitizer");
#endif

    // A 24-byte struct is not inlineable, so every value of this type is a
    // ref-counted pool cell: the tick exercises the cell path (make, retain,
    // release) and must still stay off the system heap.
    const auto network
    {
        shell::input<pooled, kernel::clock_tag::audio>("p")
        >> shell::output<pooled, kernel::clock_tag::audio>("out")
    };

    kernel::program program { network };
    kernel::release_engine engine;

    program.set<pooled>("p", pooled { 1, 2, 3 });

    g_counting = true;
    g_allocations = 0;

    engine.step(program, kernel::clock_tag::audio);

    g_counting = false;

    REQUIRE(g_allocations == 0);
    REQUIRE(kernel::get<pooled>(program, "out").a == 1);
}

TEST_CASE("kernel | A throwing tick adds no heap allocation", "[kernel]")
{
#if !CLOCKR_TEST_ALLOC_HOOK
    SKIP("allocation counting is unavailable under ThreadSanitizer");
#endif

    // The release engine rethrows the node's exception after closing the
    // region bracket. `throw 42` carries no payload of its own, so anything
    // counted here would be the engine allocating on the throwing path.
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::audio>("v")
        >> shell::lift<std::int64_t, std::int64_t>(throwing_tick)
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::program program { network };
    kernel::release_engine engine;

    program.set("v", std::int64_t { 1 });

    bool threw { false };

    g_counting = true;
    g_allocations = 0;

    try
    {
        engine.step(program, kernel::clock_tag::audio);
    }
    catch (int)
    {
        threw = true;
    }

    g_counting = false;

    REQUIRE(threw);
    REQUIRE(g_allocations == 0);
}

namespace
{
    // Hand-built graphs are gone (the shell is the only way to build graphs),
    // except the delay-equation descriptor above, which needs the bare delay
    // primitive. This helper drives one tick against a borrowed descriptor.
    kernel::value
    evaluate_once(
        kernel::engine& e,
        const kernel::descriptor_view& g,
        kernel::value in,
        std::span<kernel::delay_state> state
    )
    {
        const std::vector<kernel::value> inputs { in };

        kernel::program program { g };
        // Sync external state into the region owning the delay for this tick,
        // and back out afterwards, so repeated calls with the same state span
        // evolve correctly. (All graphs here are single-region ui.)
        auto& delays { program.region(kernel::clock_tag::ui).delay_states };

        if (!state.empty() && !delays.empty())
            delays[0] = state[0];

        program.set(0, inputs[0]);

        e.step(program, kernel::clock_tag::ui);

        if (!state.empty() && !delays.empty())
            state[0] = delays[0];

        return program.get(0);
    }
} // namespace

namespace
{
// Records what a step hook observed, and the trace length at each call so a
// test can pin the order relative to the tick's trace events.
class hook_probe final : public kernel::step_hook
{
public:
    struct call
    {
        kernel::clock_tag clock;
        conc::epoch_id epoch;
        bool threw;
    };

    // When set, the probe samples the trace string's size at each call.
    const std::string* trace { nullptr };

    std::vector<call> before;
    std::vector<call> after;
    std::size_t trace_size_at_before { 0 };
    std::size_t trace_size_at_after { 0 };

    void
    before_step(kernel::program& program, kernel::clock_tag clock) override
    {
        before.push_back(
            call { clock,
                   program.region(clock).published_epoch.load(
                       std::memory_order_acquire
                   ),
                   false }
        );

        if (trace != nullptr)
            trace_size_at_before = trace->size();
    }

    void
    after_step(
        kernel::program&,
        kernel::clock_tag clock,
        conc::epoch_id epoch,
        bool threw
    ) override
    {
        after.push_back(call { clock, epoch, threw });

        if (trace != nullptr)
            trace_size_at_after = trace->size();
    }
};

// A hook with nothing to record: used where a test only needs "something is
// attached" (it must not allocate, so the no-allocation assertion holds).
class counting_hook final : public kernel::step_hook
{
public:
    std::size_t before_count { 0 };
    std::size_t after_count { 0 };

    void
    before_step(kernel::program&, kernel::clock_tag) override
    {
        ++before_count;
    }

    void
    after_step(
        kernel::program&,
        kernel::clock_tag,
        conc::epoch_id,
        bool
    ) override
    {
        ++after_count;
    }
};

// Stands in for the debugger's drain: writes an input from the hook, which
// the step being bracketed must consume.
class writing_hook final : public kernel::step_hook
{
public:
    void
    before_step(kernel::program& program, kernel::clock_tag clock) override
    {
        program.set(0, kernel::value { std::int64_t { 21 } });
        (void)clock;
    }

    void
    after_step(
        kernel::program&,
        kernel::clock_tag,
        conc::epoch_id,
        bool
    ) override
    {}
};
} // namespace

TEST_CASE("kernel | A step hook brackets every tick exactly once", "[kernel]")
{
    const auto network { build_sum_network() };

    std::string trace;
    kernel::text_trace_sink sink { trace };

    hook_probe probe;
    probe.trace = &trace;

    kernel::debug_engine engine { sink };
    engine.set_step_hook(&probe);

    REQUIRE(engine.get_step_hook() == &probe);

    kernel::program program { network };

    program.set("a", 1);
    program.set("b", 2);

    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(probe.before.size() == 1);
    REQUIRE(probe.after.size() == 1);

    // Both calls name the region being ticked; the after epoch is the one the
    // region published for this tick.
    REQUIRE(probe.before[0].clock == kernel::clock_tag::audio);
    REQUIRE(probe.after[0].clock == kernel::clock_tag::audio);
    REQUIRE(probe.after[0].epoch == 1);
    REQUIRE(!probe.after[0].threw);

    REQUIRE(
        probe.after[0].epoch ==
        program.region(kernel::clock_tag::audio)
            .published_epoch.load(std::memory_order_acquire)
    );

    // Ordering: before_step is the first statement of the tick (nothing has
    // been traced yet) and after_step is the last (the whole tick's trace is
    // already written, including its region end).
    REQUIRE(probe.trace_size_at_before == 0);
    REQUIRE(probe.trace_size_at_after == trace.size());
    REQUIRE(trace.find("\"kind\":\"region_step_end\"") != std::string::npos);

    // A second tick is bracketed again, with the next epoch.
    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(probe.before.size() == 2);
    REQUIRE(probe.after.size() == 2);
    REQUIRE(probe.after[1].epoch == 2);
}

TEST_CASE(
    "kernel | A throwing tick reports the attempted epoch to the step hook",
    "[kernel]"
)
{
    const auto network {
        shell::input<std::int64_t, kernel::clock_tag::audio>("in") >>
        shell::lift<std::int64_t, std::int64_t>(
            [](const std::int64_t& value) -> std::int64_t {
                if (value > 10)
                    throw std::runtime_error { "too big" };

                return value;
            }
        ) >>
        shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    hook_probe probe;

    kernel::release_engine engine;
    engine.set_step_hook(&probe);

    kernel::program program { network };

    program.set("in", 99);

    REQUIRE_THROWS_AS(
        engine.step(program, kernel::clock_tag::audio), std::runtime_error
    );

    // Exactly one pair, still: the aborting tick is reported, with threw=true
    // and the attempted epoch (a throwing tick publishes no epoch, so this is
    // the only report of which tick failed).
    REQUIRE(probe.before.size() == 1);
    REQUIRE(probe.after.size() == 1);
    REQUIRE(probe.after[0].threw);
    REQUIRE(probe.after[0].epoch == 1);

    REQUIRE(
        program.region(kernel::clock_tag::audio)
            .published_epoch.load(std::memory_order_acquire) == 0
    );

    // The next (clean) tick keeps the epoch numbering honest: the aborted
    // attempt did not publish, so it is attempted again as epoch 1.
    program.set("in", 3);
    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(probe.after.size() == 2);
    REQUIRE(!probe.after[1].threw);
    REQUIRE(probe.after[1].epoch == 1);
    REQUIRE(kernel::get<std::int64_t>(program, "out") == 3);
}

TEST_CASE(
    "kernel | A write from before_step is consumed by the same tick",
    "[kernel]"
)
{
    const auto network {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in") >>
        shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    writing_hook hook;

    kernel::release_engine engine;
    engine.set_step_hook(&hook);

    kernel::program program { network };

    engine.step(program, kernel::clock_tag::ui);

    // The hook ran before the tick touched the region, so the value it set is
    // exactly the state this tick consumed (the debugger's injection anchor).
    REQUIRE(kernel::get<std::int64_t>(program, "out") == 21);
}

TEST_CASE(
    "kernel | Release and debug engines run the step hook identically",
    "[kernel]"
)
{
    const auto network { build_sum_network() };

    hook_probe release_probe;
    hook_probe debug_probe;

    std::string trace;
    kernel::text_trace_sink sink { trace };

    kernel::release_engine release;
    kernel::debug_engine debug { sink };

    release.set_step_hook(&release_probe);
    debug.set_step_hook(&debug_probe);

    kernel::program release_program { network };
    kernel::program debug_program { network };

    for (auto* program : { &release_program, &debug_program })
    {
        program->set("a", 4);
        program->set("b", 5);
    }

    release.step(release_program, kernel::clock_tag::audio);
    debug.step(debug_program, kernel::clock_tag::audio);

    // Picking the engine type is a construction-time choice; the hook sees the
    // same bracketing either way.
    REQUIRE(release_probe.before.size() == 1);
    REQUIRE(debug_probe.before.size() == 1);
    REQUIRE(release_probe.after.size() == 1);
    REQUIRE(debug_probe.after.size() == 1);
    REQUIRE(release_probe.after[0].clock == debug_probe.after[0].clock);
    REQUIRE(release_probe.after[0].epoch == debug_probe.after[0].epoch);
    REQUIRE(release_probe.after[0].threw == debug_probe.after[0].threw);

    REQUIRE(release_program.get(0) == debug_program.get(0));
    REQUIRE(kernel::get<std::int64_t>(release_program, "out") == 9);
}

TEST_CASE("kernel | Detaching the step hook stops the callbacks", "[kernel]")
{
    const auto network { build_sum_network() };

    counting_hook hook;

    kernel::release_engine engine;
    engine.set_step_hook(&hook);

    kernel::program program { network };

    program.set("a", 1);
    program.set("b", 2);

    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(hook.before_count == 1);
    REQUIRE(hook.after_count == 1);

    engine.set_step_hook(nullptr);

    REQUIRE(engine.get_step_hook() == nullptr);

    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(hook.before_count == 1);
    REQUIRE(hook.after_count == 1);
}

TEST_CASE(
    "kernel | A step hook adds no heap allocation to the step path",
    "[kernel]"
)
{
#if !CLOCKR_TEST_ALLOC_HOOK
    SKIP("allocation counting is unavailable under ThreadSanitizer");
#endif

    const auto network { build_sum_network() };

    counting_hook hook;

    kernel::release_engine engine;
    engine.set_step_hook(&hook);

    kernel::program program { network };

    program.set("a", 6);
    program.set("b", 7);

    // Warm up the pools/rings first; the measured tick must not allocate.
    engine.step(program, kernel::clock_tag::audio);

    g_counting = true;
    g_allocations = 0;

    engine.step(program, kernel::clock_tag::audio);

    g_counting = false;

    REQUIRE(g_allocations == 0);
    REQUIRE(hook.before_count == 2);
    REQUIRE(hook.after_count == 2);
    REQUIRE(kernel::get<std::int64_t>(program, "out") == 13);
}
