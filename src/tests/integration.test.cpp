#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/kernel/trace.hpp"

#include "clockr/kernel/graph.hpp"

#include "clockr/shell/recipe.hpp"

#include "clockr/mem/arena.hpp"
#include "clockr/mem/pool.hpp"
#include "clockr/mem/pool_allocator.hpp"

#include "clockr/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// This suite consumes the library the way client code does: it links only the
// public clockr::clockr umbrella target and includes public headers
// relative to src/. It is meant to exercise the modules composing a
// real program (recipe wiring, engine evaluation, and pool-backed storage)
// rather than any single module in isolation. Cross-thread hand-off is
// covered in conc's own tests and kernel's region_tick suite, not here.

namespace
{
    using namespace clockr;

    auto
    mult(double multiplier)
    {
        return shell::lift<double, double>([multiplier](const double& v) { return v * multiplier; });
    }
} // namespace

TEST_CASE("integration | A graph is wired and evaluated end to end", "[integration]")
{
    using namespace clockr::kernel;

    // Hand-built kernel graphs are gone (the shell lowers recipes now).
    const auto network
    {
        shell::zip(
            shell::input<std::int64_t, kernel::clock_tag::audio>("left"),
            shell::input<std::int64_t, kernel::clock_tag::audio>("right"))
            >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
                   [](const std::int64_t& x, const std::int64_t& y) { return x + y; })
            >> shell::lift<std::int64_t, bool>([](const std::int64_t& v) { return v > 10; })
            >> shell::output<bool, kernel::clock_tag::audio>("out")
    };

    release_engine engine;
    kernel::program program { network };

    std::vector<kernel::value> inputs { kernel::value { std::int64_t { 6 } }, kernel::value { std::int64_t { 7 } } };

    // Set up the program with inputs
    program.set(0, inputs[0]);
    program.set(1, inputs[1]);

    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(std::get<bool>(program.get(0)));
    REQUIRE(std::get<std::int64_t>(inputs[0]) + std::get<std::int64_t>(inputs[1]) == 13);
}

TEST_CASE("integration | A pool backs the buffers flowing through an engine", "[integration]")
{
    using namespace clockr::kernel;

    mem::pool storage { mem::arena { 64 * 1024 } };

    std::vector<kernel::value, mem::pool_allocator<kernel::value>> inputs { mem::pool_allocator<kernel::value> { storage } };
    std::vector<kernel::value, mem::pool_allocator<kernel::value>> outputs { mem::pool_allocator<kernel::value> { storage } };

    inputs.emplace_back(kernel::value { std::int64_t { 9 } });
    inputs.emplace_back(kernel::value { std::int64_t { 5 } });
    outputs.emplace_back();

    const auto network
    {
        shell::zip(
            shell::input<std::int64_t, kernel::clock_tag::audio>("a"),
            shell::input<std::int64_t, kernel::clock_tag::audio>("b"))
            >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
                   [](const std::int64_t& x, const std::int64_t& y) { return x + y; })
            >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    release_engine engine;
    kernel::program program { network };

    // Set up the program with inputs
    program.set(0, inputs[0]);
    program.set(1, inputs[1]);

    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(std::get<std::int64_t>(program.get(0)) == 14);
    REQUIRE(storage.owns(&inputs[0]));
    REQUIRE(storage.check());
}

TEST_CASE("integration | The umbrella target exposes the generated version header", "[integration]")
{
    REQUIRE(std::string_view(version::str) == "0.1.0");
}

TEST_CASE("integration/playground | A recipe network instantiates into a program and evaluates", "[integration]")
{
    using namespace clockr::kernel;

    // A network is a composable recipe
    const auto network
    {
        shell::input<double, kernel::clock_tag::ui>("in")
            >> mult(2)
            >> shell::output<double, kernel::clock_tag::ui>("out")
    };

    // A program carries an instantiated network, its named inputs and outputs, and its running state.
    kernel::program program { network };

    program.set("in", kernel::value { 21.0 });

    // The engine evaluates
    release_engine engine;
    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(std::get<double>(program.get("out")) == get<double>(program, "out"));
    REQUIRE(get<double>(program, "out") == 42.0);
}
