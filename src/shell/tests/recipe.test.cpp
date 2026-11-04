#include "clockr/shell/recipe.hpp"
#include "clockr/shell/builder.hpp"

#include "clockr/kernel/release_engine.hpp"

#include "clockr/kernel/topology.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm> // std::ranges::equal
#include <cstdint>
#include <variant>
#include <vector>

using namespace clockr;

namespace
{
    auto
    plus_one()
    {
        return shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; });
    }

    auto
    times_two()
    {
        return shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x * 2; });
    }

    template<kernel::graph_c graph_t>
    std::size_t
    count_kind(const graph_t& g, kernel::node_kind kind);
} // namespace

TEST_CASE("shell | Value conversion is permissive in, strict out", "[recipe]")
{
    REQUIRE(std::holds_alternative<std::int64_t>(shell::to_value(std::int64_t { 42 })));
    REQUIRE(std::holds_alternative<std::int64_t>(shell::to_value(42)));      // int literal
    REQUIRE(std::holds_alternative<double>(shell::to_value(2.5f)));          // float literal
    REQUIRE(std::holds_alternative<bool>(shell::to_value(true)));

    REQUIRE(shell::from_value<std::int64_t>(kernel::value { std::int64_t { 42 } }) == 42);
    REQUIRE_THROWS_AS(shell::from_value<bool>(kernel::value { std::int64_t { 1 } }), std::bad_variant_access);
}

TEST_CASE("shell | Absence converts through optional ports, strict on mismatch", "[recipe]")
{
    // Engaged payloads convert like bare values; disengaged convert to the
    // empty value; reading mirrors it. A genuine mismatch still throws.
    REQUIRE(shell::to_value(std::optional<double>{2.5}) == kernel::value{2.5});
    REQUIRE(shell::to_value(std::optional<double>{}) == kernel::value{});
    REQUIRE(shell::to_value(std::nullopt) == kernel::value{});

    REQUIRE(shell::from_value<std::optional<double>>(kernel::value{2.5}) == std::optional<double>{2.5});
    REQUIRE(!shell::from_value<std::optional<double>>(kernel::value{}).has_value());
    REQUIRE_THROWS_AS(shell::from_value<std::optional<double>>(kernel::value{true}), std::bad_variant_access);
    REQUIRE_THROWS_AS(shell::from_value<std::optional<bool>>(kernel::value{std::int64_t{1}}), std::bad_variant_access);
}

TEST_CASE("shell | An event head absorbs absence without infecting downstream types", "[recipe]")
{
    // The containment pattern: optional at the boundary, plain T past it. A
    // missing event holds the last beat; downstream never sees Maybe — note
    // the first tick, which reads the never-set input as disengaged instead
    // of throwing (the pre-publish case that partial lifts cannot survive).
    const auto network
    {
        shell::input<std::optional<double>, kernel::clock_tag::ui>("seek")
        >> shell::scan<std::optional<double>, double>(
               0.0,
               [](const double& prev, const std::optional<double>& m) { return m.value_or(prev); })
        >> shell::output<double, kernel::clock_tag::ui>("pos")
    };

    kernel::program program { network };

    kernel::release_engine engine;

    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 0.0);

    program.set("seek", shell::to_value(std::optional<double>{4.0}));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 4.0);

    program.set("seek", shell::to_value(std::nullopt));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 4.0);

    program.set("seek", shell::to_value(std::optional<double>{7.0}));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 7.0);
}

TEST_CASE("shell | stepper seeds, holds and adopts firings", "[recipe]")
{
    // Seed, fire, silence, silence, re-fire: the seed shows before the first
    // firing (the pre-publish totality case — nothing throws), then the output
    // repeats the last firing across the silent ticks. That repeat is what
    // makes this operator right for levels and wrong for event channels,
    // where edge-triggered consumers would re-fire.
    const auto network
    {
        shell::input<std::optional<double>, kernel::clock_tag::ui>("seek")
        >> shell::stepper<double>(0.0)
        >> shell::output<double, kernel::clock_tag::ui>("pos")
    };

    kernel::program program { network };

    kernel::release_engine engine;

    // No set yet: the seed shows, nothing throws.
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 0.0);

    program.set("seek", shell::to_value(std::optional<double>{4.0}));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 4.0);

    program.set("seek", shell::to_value(std::nullopt));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 4.0);

    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 4.0);

    program.set("seek", shell::to_value(std::optional<double>{9.0}));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "pos") == 9.0);
}

TEST_CASE("shell | stepper is scan with value_or spelled out", "[recipe]")
{
    // Guards the sugar against drift: sugar and manual expansion agree over a
    // scripted sequence covering seed, adopt, carry, and re-fire.
    const auto sugared
    {
        shell::input<std::optional<std::int64_t>, kernel::clock_tag::ui>("s")
        >> shell::stepper<std::int64_t>(-1)
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("o")
    };
    const auto manual
    {
        shell::input<std::optional<std::int64_t>, kernel::clock_tag::ui>("s")
        >> shell::scan<std::optional<std::int64_t>, std::int64_t>(
               -1,
               [](const std::int64_t& prev, const std::optional<std::int64_t>& m)
               {
                   return m.value_or(prev);
               })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("o")
    };

    kernel::program via_sugar { sugared };
    kernel::program via_manual { manual };

    kernel::release_engine engine;

    const auto both_tick = [&](std::optional<std::int64_t> firing)
    {
        via_sugar.set("s", shell::to_value(firing));
        via_manual.set("s", shell::to_value(firing));
        engine.step(via_sugar, kernel::clock_tag::ui);
        engine.step(via_manual, kernel::clock_tag::ui);
        REQUIRE(get<std::int64_t>(via_sugar, "o") == get<std::int64_t>(via_manual, "o"));
    };

    both_tick(std::nullopt);
    REQUIRE(get<std::int64_t>(via_sugar, "o") == -1);

    both_tick(std::optional<std::int64_t>{3});
    REQUIRE(get<std::int64_t>(via_sugar, "o") == 3);

    both_tick(std::nullopt);
    REQUIRE(get<std::int64_t>(via_sugar, "o") == 3);

    both_tick(std::optional<std::int64_t>{8});
    REQUIRE(get<std::int64_t>(via_sugar, "o") == 8);
}

TEST_CASE("shell | Event inputs expire after the tick that consumes them", "[recipe]")
{
    // Fire-and-forget: an optional input holds its firing for exactly one
    // tick, then reverts to disengaged. The engine clears it post-settle;
    // nothing downstream has to remember to forget.
    const auto network
    {
        shell::input<std::optional<std::int64_t>, kernel::clock_tag::ui>("fire")
        >> shell::output<std::optional<std::int64_t>, kernel::clock_tag::ui>("fired")
    };

    kernel::program program { network };

    kernel::release_engine engine;

    // Never set: disengaged from the start, no throw.
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(!shell::from_value<std::optional<std::int64_t>>(program.get("fired")).has_value());

    program.set("fire", shell::to_value(std::optional<std::int64_t>{5}));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(shell::from_value<std::optional<std::int64_t>>(program.get("fired")) == std::optional<std::int64_t>{5});

    // No new set: expired back to disengaged (would have stayed Just(5)
    // under level semantics).
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(!shell::from_value<std::optional<std::int64_t>>(program.get("fired")).has_value());

    program.set("fire", shell::to_value(std::optional<std::int64_t>{7}));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(shell::from_value<std::optional<std::int64_t>>(program.get("fired")) == std::optional<std::int64_t>{7});
}

TEST_CASE("shell | Event expiry never touches neighbouring level ports", "[recipe]")
{
    // The storage rule follows the port type: the optional chain fires once
    // and expires, while the plain chain next to it persists untouched.
    const auto network
    {
        shell::parallel(
            shell::input<std::optional<std::int64_t>, kernel::clock_tag::ui>("fire")
                >> shell::output<std::optional<std::int64_t>, kernel::clock_tag::ui>("fired"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("level")
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("held")
        )
    };

    kernel::program program { network };

    kernel::release_engine engine;

    program.set("fire", shell::to_value(std::optional<std::int64_t>{5}));
    program.set("level", 9);
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(shell::from_value<std::optional<std::int64_t>>(program.get("fired")) == std::optional<std::int64_t>{5});
    REQUIRE(get<std::int64_t>(program, "held") == 9);

    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(!shell::from_value<std::optional<std::int64_t>>(program.get("fired")).has_value());
    REQUIRE(get<std::int64_t>(program, "held") == 9);
}

TEST_CASE("shell | Values share their immutable recipe", "[recipe]")
{
    const auto a { plus_one() };
    const auto b { a };

    REQUIRE(a.root() == b.root());
}

TEST_CASE("shell | A lifted chain instantiates and evaluates", "[recipe]")
{
    // (Structural parity for lowered recipes lives in program.test.cpp's
    // owning/borrowing cases; here remains instantiation + evaluation.)
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::audio>("in")
            >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; })
            >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x * 2; })
            >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::program program { network };
    program.set("in", kernel::value { 41 });

    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::audio);

    REQUIRE(kernel::get<std::int64_t>(program, "out") == 84);
}

TEST_CASE("shell | Port tags place nodes, and topology sees the thread boundary", "[recipe]")
{
    // Threads are anchored at ports, not given to build(): two independent
    // chains under parallel each derive their clock from their own tags.
    const auto ui_chain
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
        >> plus_one()
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };
    const auto audio_chain
    {
        shell::input<std::int64_t, kernel::clock_tag::audio>("gain")
        >> times_two()
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out_gain")
    };
    const auto network { parallel(ui_chain, audio_chain) };

    kernel::graph g { build(network) };

    REQUIRE(clock(at(g, 0)) == kernel::clock_tag::ui);
    REQUIRE(clock(at(g, 1)) == kernel::clock_tag::ui);
    REQUIRE(clock(at(g, 2)) == kernel::clock_tag::ui);
    REQUIRE(clock(at(g, 3)) == kernel::clock_tag::audio);
    REQUIRE(clock(at(g, 4)) == kernel::clock_tag::audio);
    REQUIRE(clock(at(g, 5)) == kernel::clock_tag::audio);

    const shell::flat_map<kernel::node_id, conc::policy> crossings;
    const kernel::topology t { g, crossings };

    REQUIRE(t.regions.size() == 2);
    // Cross-region edges exist only through an explicit resampler (C1); a
    // graph anchored at ports alone has none.
    REQUIRE(t.crossings.size() == 0);
}

TEST_CASE("shell | A network with no anchor anywhere is rejected", "[recipe]")
{
    // Dead code: nothing feeds it and nothing reads it, so no clock can be
    // derived — build() fails instead of guessing.
    const auto network { plus_one() };

    REQUIRE_THROWS_AS(build(network), std::runtime_error);
}

TEST_CASE("shell | A head-less fragment has no clock to derive and is rejected", "[recipe]")
{
    // Outputs anchor as *checks*, not seeds: data enters a fragment at a
    // tagged input, so a chain that starts in the middle (a constant
    // generator with no upstream) has no clock that can be derived forward.
    // Build fails with a clear message instead of a confusing fan-in error.
    const auto network
    {
        shell::lift<double, double>([](const double&) { return 440.0; })
        >> shell::output<double, kernel::clock_tag::audio>("pitch")
    };

    REQUIRE_THROWS_AS(build(network), std::runtime_error);
}

TEST_CASE("shell | Crossing a clock without a crossing is a build-time error", "[recipe]")
{
    // C1's strict pin, reachable before at<> exists: the input anchors ui,
    // the output anchors audio, and nothing in between states how the value
    // crosses — error.
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
        >> plus_one()
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    REQUIRE_THROWS_AS(build(network), std::runtime_error);
}

TEST_CASE("shell | Contradicting port anchors in one branch are rejected", "[recipe]")
{
    // The ports of a single linear chain must agree: the interior between
    // them runs on exactly one clock.
    const auto network
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("a") >> plus_one() >> shell::output<std::int64_t, kernel::clock_tag::ui>("a_out"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("b") >> times_two() >> shell::output<std::int64_t, kernel::clock_tag::audio>("b_out")
        )
    };

    REQUIRE_THROWS_AS(build(network), std::runtime_error);
}

TEST_CASE("shell | Sharing the same fragment on two clocks instantiates it twice", "[recipe]")
{
    // share() is "once per (recipe, clock)": a clock-less shared fragment fed
    // on ui and on audio becomes two nodes, one per clock — silently sharing a
    // single node across clocks would be wrong.
    const auto network
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("a") >> shell::share(plus_one()) >> shell::output<std::int64_t, kernel::clock_tag::ui>("a_out"),
            shell::input<std::int64_t, kernel::clock_tag::audio>("b") >> shell::share(plus_one()) >> shell::output<std::int64_t, kernel::clock_tag::audio>("b_out")
        )
    };

    kernel::graph g { build(network) };

    REQUIRE(count_kind(g, kernel::node_kind::internal) == 2);

    // One instantiation per clock: a lift on ui and a lift on audio.
    bool ui_internal { false };
    bool audio_internal { false };

    for (const auto& n : nodes(g))
        if (kind(n) == kernel::node_kind::internal)
        {
            ui_internal |= clock(n) == kernel::clock_tag::ui;
            audio_internal |= clock(n) == kernel::clock_tag::audio;
        }

    REQUIRE(ui_internal);
    REQUIRE(audio_internal);
}

TEST_CASE("shell | A share at one source location cannot span two input ports", "[recipe]")
{
    // share()'s id is nominal (its source location), so a helper parameterized
    // by port name reuses one id for two distinct ports and the memo would
    // silently collapse the second onto the first. The walk must reject it.
    const auto channel = [](std::string_view name)
    {
        return shell::share(shell::input<std::int64_t, kernel::clock_tag::ui>(name));
    };

    const auto network
    {
        shell::parallel(
            channel("left") >> plus_one() >> shell::output<std::int64_t, kernel::clock_tag::ui>("left_out"),
            channel("right") >> times_two() >> shell::output<std::int64_t, kernel::clock_tag::ui>("right_out")
        )
    };

    REQUIRE_THROWS_AS(build(network), std::runtime_error);
}

TEST_CASE("shell | A parameterless share helper dedups across call sites", "[recipe]")
{
    // The other half of the nominal-id rule: one share() token called from two
    // places (a parameterless helper) is one id, so the port is emitted once.
    const auto playing = []
    {
        return shell::share(shell::input<std::int64_t, kernel::clock_tag::ui>("playing"));
    };

    const auto network
    {
        shell::parallel(
            playing() >> plus_one() >> shell::output<std::int64_t, kernel::clock_tag::ui>("a"),
            playing() >> times_two() >> shell::output<std::int64_t, kernel::clock_tag::ui>("b")
        )
    };

    kernel::graph g { build(network) };

    REQUIRE(count_kind(g, kernel::node_kind::input) == 1);
}

TEST_CASE("shell | A recipe instantiates twice into independent programs", "[recipe]")
{
    // The same recipe value is a reusable description: instantiating it twice
    // yields two independent programs, each with its own storage. The share
    // memoization lives in the per-build draft, so it cannot leak across builds.
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::audio>("in")
            >> plus_one()
            >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::release_engine engine;

    const std::vector<kernel::value> inputs_1 { kernel::value { std::int64_t { 1 } } };
    const std::vector<kernel::value> inputs_2 { kernel::value { std::int64_t { 2 } } };

    kernel::program prog1 { network };
    prog1.set("in", inputs_1[0]);
    engine.step(prog1, kernel::clock_tag::audio);

    kernel::program prog2 { network };
    prog2.set("in", inputs_2[0]);
    engine.step(prog2, kernel::clock_tag::audio);

    REQUIRE(std::get<std::int64_t>(prog1.get("out")) == 2);
    REQUIRE(std::get<std::int64_t>(prog2.get("out")) == 3);
}

TEST_CASE("shell | Scan accumulates across ticks", "[A2][recipe]")
{ // §3.1: scan is a pure step function fed through delay; out(t) = f(s(t-1), in(t)),
  // with s(-1) = init participating in the very first tick.
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
        >> shell::scan<std::int64_t, std::int64_t>(
               10,
               [](const std::int64_t& prev, const std::int64_t& in) { return prev + in; }
           )
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };

    kernel::release_engine engine;

    // First tick: f(init, in) = 10 + 1 — the seed participates immediately.
    program.set("in", 1);
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "out") == 11);

    program.set("in", 2);
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "out") == 13);

    program.set("in", 3);
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "out") == 16);
}

TEST_CASE("shell | Fan-in order is zip order", "[B1][recipe]")
{ // L8 (pairing + order clause): a pair-typed output is two fan-in edges, in
  // zip order. Subtraction catches an accidental swap of the two zipped ports
  // (an addition-only check would pass either way).
    const auto network
    {
        shell::zip(
            shell::input<std::int64_t, kernel::clock_tag::ui>("in_1"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("in_2")
        )
        >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
               [](const std::int64_t& a, const std::int64_t& b) { return a - b; }
           )
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };

    kernel::release_engine engine;

    program.set("in_1", 6);
    program.set("in_2", 7);

    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(get<std::int64_t>(program, "out") == -1);
}

TEST_CASE("shell | Zip feeds N branches to one lift", "[B1][recipe]")
{ // L8 generalized: a nested-pair wire is one fan-in edge per leaf, left to
  // right. Positional encoding catches any mis-ordering of the three edges.
    const auto network
    {
        shell::zip(
            shell::input<std::int64_t, kernel::clock_tag::ui>("in_1"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("in_2"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("in_3")
        )
        >> shell::lift<std::pair<std::pair<std::int64_t, std::int64_t>, std::int64_t>, std::int64_t>(
               [](const std::int64_t& a, const std::int64_t& b, const std::int64_t& c)
               { return a * 100 + b * 10 + c; }
           )
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };

    kernel::release_engine engine;

    program.set("in_1", 1);
    program.set("in_2", 2);
    program.set("in_3", 3);

    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(get<std::int64_t>(program, "out") == 123);
}

TEST_CASE("shell | Delay seeds its output then shifts by one tick", "[recipe]")
{ // The delay primitive: out(0) is the seed, out(t) is in(t-1) — the state
  // half of scan, exposed standalone. The third tick shows the level input
  // persisting: with "in" still 7, the output repeats 7.
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
        >> shell::delay<std::int64_t>(0)
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };

    kernel::release_engine engine;

    program.set("in", 5);
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "out") == 0);

    program.set("in", 7);
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "out") == 5);

    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "out") == 7);
}

TEST_CASE("shell | A diamond reconverges without glitch", "[B1][recipe]")
{
    // L2 (synchrony): one source, two paths, one join — both paths see the
    // same tick's value. share() makes the source a single input node.
    const auto source { shell::share(shell::input<std::int64_t, kernel::clock_tag::ui>("in")) };

    const auto network
    {
        shell::zip(
            source >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x * 2; }),
            source >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 10; })
        )
        >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
               [](const std::int64_t& a, const std::int64_t& b) { return a + b; }
           )
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };

    // Structural: the shared source is a single input node.
    REQUIRE(count_kind(program.descriptor(), kernel::node_kind::input) == 1);

    kernel::release_engine engine;

    program.set("in", 41);

    engine.step(program, kernel::clock_tag::ui);

    // (41 * 2) + (41 + 10) = 133
    REQUIRE(get<std::int64_t>(program, "out") == 133);
}

TEST_CASE("shell | Zip forwards its shared upstream to both branches", "[B1][recipe]")
{
    // L8 (pairing): the zip's upstream type X is fanned out to every branch —
    // here X = int64_t, so one source feeds both, no share() needed.
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
        >> shell::zip(plus_one(), times_two())
        >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
               [](const std::int64_t& a, const std::int64_t& b) { return a + b; }
           )
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };

    // Structural: a single source fans out to both branches.
    REQUIRE(count_kind(program.descriptor(), kernel::node_kind::input) == 1);

    kernel::release_engine engine;

    program.set("in", 5);

    engine.step(program, kernel::clock_tag::ui);

    // (5 + 1) + (5 * 2) = 16
    REQUIRE(get<std::int64_t>(program, "out") == 16);
}

TEST_CASE("shell | Zips under parallel are independent", "[B1][recipe]")
{
    // Two independent zip→lift networks in one parallel: each keeps its own
    // branches and its own fan-in order (L8); nothing leaks across children.
    const auto network
    {
        shell::parallel(
            shell::zip(shell::input<std::int64_t, kernel::clock_tag::ui>("a1"), shell::input<std::int64_t, kernel::clock_tag::ui>("b1"))
                >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
                       [](const std::int64_t& a, const std::int64_t& b) { return a + b; }
                   )
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("o1"),
            shell::zip(shell::input<std::int64_t, kernel::clock_tag::ui>("a2"), shell::input<std::int64_t, kernel::clock_tag::ui>("b2"))
                >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
                       [](const std::int64_t& a, const std::int64_t& b) { return a * b; }
                   )
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("o2")
        )
    };

    kernel::program program { network };

    REQUIRE(count_kind(program.descriptor(), kernel::node_kind::input) == 4);

    kernel::release_engine engine;

    program.set("a1", 6);
    program.set("b1", 7);
    program.set("a2", 3);
    program.set("b2", 4);

    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(get<std::int64_t>(program, "o1") == 13); // 6 + 7
    REQUIRE(get<std::int64_t>(program, "o2") == 12); // 3 * 4
}

TEST_CASE("shell | A shared zip is instantiated once and feeds both outputs", "[B1][recipe]")
{
    // L4 (share-identity) extended to a multi-output fragment: share() of a
    // zip→lift network memoizes the whole output span, so the zip's branches
    // are instantiated once while both consumers fan out from the shared node.
    const auto zipped
    {
        shell::share(
            shell::zip(shell::input<std::int64_t, kernel::clock_tag::ui>("a"), shell::input<std::int64_t, kernel::clock_tag::ui>("b"))
            >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
                   [](const std::int64_t& a, const std::int64_t& b) { return a + b; }
               )
        )
    };

    const auto network
    {
        shell::parallel(
            zipped >> shell::output<std::int64_t, kernel::clock_tag::ui>("o1"),
            zipped >> shell::output<std::int64_t, kernel::clock_tag::ui>("o2")
        )
    };

    kernel::program program { network };

    // Structural: the zip's branches (a, b) are instantiated exactly once.
    REQUIRE(count_kind(program.descriptor(), kernel::node_kind::input) == 2);

    kernel::release_engine engine;

    program.set("a", 6);
    program.set("b", 7);

    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(get<std::int64_t>(program, "o1") == 13);
    REQUIRE(get<std::int64_t>(program, "o2") == 13);
}

TEST_CASE("shell | A resample annotation marks its crossing", "[C1][recipe][topology]")
{
    const auto bpm { shell::input<double, kernel::clock_tag::ui>("bpm") }; // lives on the ui clock

    const auto network
    {
        shell::at<kernel::clock_tag::audio, conc::policy::latest>(bpm)
            >> shell::lift<double, double>([](const double& v) { return v * 2; })
            >> shell::output<double, kernel::clock_tag::audio>("out")
    };

    // bpm anchors ui; at<> crosses to audio and re-anchors the walker: the
    // lift and the output run on audio.
    shell::draft draft;
    std::optional<kernel::clock_tag> clock;
    shell::instantiate(draft, clock, network);

    kernel::graph g { shell::build(network) };

    const kernel::topology t { g, draft.context.crossing_map };

    REQUIRE(t.crossings.size() == 1);
    REQUIRE(t.crossings.front().policy == conc::policy::latest); // assumed accessor
}

TEST_CASE("shell | An un-annotated cross-region edge is a hard error", "[C1][recipe]")
{
    // Pins the strict choice: crossing clocks without stating a resampling
    // policy is a build-time failure. (With the boundary-anchored model this
    // is already true before at<> exists: the input anchors ui, the output
    // anchors audio, and nothing in between states the crossing.) If you
    // choose the lenient default instead, replace with REQUIRE_NOTHROW +
    // assert the defaulted mode.
    const auto network
    {
        shell::input<double, kernel::clock_tag::ui>("in")
        >> shell::lift<double, double>([](const double& v) { return v * 2; })
        >> shell::output<double, kernel::clock_tag::audio>("out")
    };

    REQUIRE_THROWS(shell::build(network));
}

TEST_CASE("shell | at<> on the clock it is already on is rejected", "[C1][recipe]")
{
    // Resampling within one clock is meaningless — better to reject than to
    // silently no-op. (Alternative pin: harmless no-op; choose explicitly.)
    // The source anchors ui, so at<ui, latest> would resample ui onto ui.
    const auto network
    {
        shell::at<kernel::clock_tag::ui, conc::policy::latest>(shell::input<double, kernel::clock_tag::ui>("in"))
        >> shell::output<double, kernel::clock_tag::ui>("out")
    };

    REQUIRE_THROWS(shell::build(network));
}

TEST_CASE("shell | L9a same port last write wins within one epoch (coalesce)", "[C1][L9][epoch]")
{
    // Same-epoch pushes coalesce to the last: setting the same port twice
    // before the tick must leave no trace of the first value. This is the
    // input analogue of §3.2's link coalescing (ŝ(i)=last).
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("x")
        >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& v){ return v * 2; })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program p { network };
    kernel::release_engine e;

    // Two sets within same epoch — only the last should be visible.
    p.set("x", std::int64_t{999});
    p.set("x", std::int64_t{21});
    e.step(p, kernel::clock_tag::ui);
    REQUIRE(kernel::get<std::int64_t>(p, "out") == 42);

    // Re-establish: single set still works.
    p.set("x", std::int64_t{10});
    e.step(p, kernel::clock_tag::ui);
    REQUIRE(kernel::get<std::int64_t>(p, "out") == 20);

    // Three overwrites in one epoch, last wins.
    p.set("x", std::int64_t{1});
    p.set("x", std::int64_t{2});
    p.set("x", std::int64_t{3});
    e.step(p, kernel::clock_tag::ui);
    REQUIRE(kernel::get<std::int64_t>(p, "out") == 6);
}

TEST_CASE("shell | L9a input epoch is a map - three ports permuted", "[C1][L9][epoch]")
{
    // L9a: an epoch contributes a map, not a sequence — all 6 set() orders
    // over three inputs must agree. Catches implementations that
    // accidentally depend on set() insertion order or on positional
    // mapping rather than name routing.
    const auto ab
    {
        shell::zip(
            shell::input<std::int64_t, kernel::clock_tag::ui>("a"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("b")
        )
        >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>([](const std::int64_t& x, const std::int64_t& y){ return x + y; })
    };
    const auto network
    {
        shell::zip(ab, shell::input<std::int64_t, kernel::clock_tag::ui>("c"))
        >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>([](const std::int64_t& s, const std::int64_t& c){ return s * 10 + c; })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
        // Effectively (a+b)*10 + c
    };

    // Reference: a=1,b=2,c=3 => (3)*10+3=33
    auto run_permutation = [&](std::array<const char*,3> order) -> std::int64_t
    {
        kernel::program p { network };
        kernel::release_engine e;
        // Set in given order
        for (auto name : order)
        {
            if (std::string_view{name} == "a") p.set("a", std::int64_t{1});
            else if (std::string_view{name} == "b") p.set("b", std::int64_t{2});
            else p.set("c", std::int64_t{3});
        }
        e.step(p, kernel::clock_tag::ui);
        return kernel::get<std::int64_t>(p, "out");
    };

    const std::int64_t ref = run_permutation({"a","b","c"});
    REQUIRE(ref == 33);
    REQUIRE(run_permutation({"a","c","b"}) == ref);
    REQUIRE(run_permutation({"b","a","c"}) == ref);
    REQUIRE(run_permutation({"b","c","a"}) == ref);
    REQUIRE(run_permutation({"c","a","b"}) == ref);
    REQUIRE(run_permutation({"c","b","a"}) == ref);
}

namespace
{
    template<kernel::graph_c graph_t>
    std::size_t
    count_kind(const graph_t& g, kernel::node_kind p_kind)
    {
        std::size_t n { 0 };

        for (const auto& node : nodes(g))
            if (kind(node) == p_kind)
                ++n;

        return n;
    }

    constexpr auto plus_one_closure = [](const std::int64_t& x) { return x + 1; };
} // namespace

static_assert(
    shell::stateless_lift_v<decltype(plus_one_closure), std::int64_t, std::int64_t>,
    "a captureless lambda is a stateless lift"
);
constexpr kernel::value one_in { std::int64_t { 1 } };
constexpr kernel::value two_out { std::int64_t { 2 } };
static_assert(
    shell::detail::stateless_tick<decltype(plus_one_closure), std::int64_t, std::int64_t>(
        nullptr,
        kernel::tick_context {},
        std::span<const kernel::value>(&one_in, 1)
    ) == two_out,
    "the stateless lift tick is constant-evaluable"
);
