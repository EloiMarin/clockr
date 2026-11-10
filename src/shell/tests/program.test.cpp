#include "clockr/shell/recipe.hpp"
#include "clockr/shell/builder.hpp"
#include "clockr/shell/descriptor_storage.hpp"

#include "clockr/kernel/release_engine.hpp"

#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/topology.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <algorithm> // std::ranges::equal
#include <cstdint>
#include <string_view>
#include <vector>

using namespace clockr;

template<>
struct Catch::StringMaker<clockr::kernel::value>
{
    static std::string
    convert(const clockr::kernel::value& value)
    {
        return std::visit(
            [](const auto& x) -> std::string
            {
                using T = std::decay_t<decltype(x)>;

                if constexpr (std::is_same_v<T, std::monostate>)
                    return "init";
                else if constexpr (std::is_same_v<T, bool>)
                    return x ? "true" : "false";
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    return std::to_string(x);
                else if constexpr (std::is_same_v<T, double>)
                    return std::to_string(x);

                return "(unknown)";
            },
            value
        );
    }
};

TEST_CASE("shell/program | Positional ports follow declaration order", "[D1][recipe]")
{ // §2.2: the positional convention becomes declaration order, observable
  // through set(i)/get(i) — no storage spans needed to inspect it.
    const auto child_1 { shell::input<std::int64_t, kernel::clock_tag::ui>("in1") >> shell::output<std::int64_t, kernel::clock_tag::ui>("out1") };
    const auto child_2 { shell::input<std::int64_t, kernel::clock_tag::ui>("in2") >> shell::output<std::int64_t, kernel::clock_tag::ui>("out2") };

    kernel::program program { shell::parallel(child_1, child_2) };

    // Declaration order, not evaluation-order coincidence: index 0 addresses
    // child_1's ports, index 1 child_2's.
    program.set(0, kernel::value { 1 });
    program.set(1, kernel::value { 2 });

    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(program.get(0) == kernel::value { 1 });
    REQUIRE(program.get(1) == kernel::value { 2 });
}

TEST_CASE("shell/program | Evaluating a program matches rehydrating it from its descriptor", "[D1][recipe]")
{
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
        >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };

    program.set("in", kernel::value { 41 });

    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);
    
    // Manually evaluate the graph to compare outputs 
    kernel::program compare_program { program.descriptor() };
    compare_program.set("in", 41);
    engine.step(compare_program, kernel::clock_tag::ui);

    REQUIRE(program.get(0) == compare_program.get(0));
    REQUIRE(program.get(0) == kernel::value { std::int64_t { 42 } });
}

TEST_CASE("shell/program | Named inputs route regardless of set order", "[D2][recipe]")
{ // §2.2 (named ports): routing by name is order-independent.
    // Two independent passthrough chains, declared a-then-b, set b-then-a.
    kernel::program program
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("a") >> shell::output<std::int64_t, kernel::clock_tag::ui>("x"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("b") >> shell::output<std::int64_t, kernel::clock_tag::ui>("y")
        )
    };

    REQUIRE_NOTHROW(program.set("b", kernel::value { 2 })); // reverse order
    REQUIRE_NOTHROW(program.set("a", kernel::value { 1 }));

    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(program.get("x") == kernel::value { 1 });
    REQUIRE(program.get("y") == kernel::value { 2 });
}

TEST_CASE("shell/program | get_or tolerates an output that has not been produced", "[recipe]")
{
    kernel::program program
    {
        shell::input<double, kernel::clock_tag::ui>("in")
            >> shell::lift<double, double>([](const double& x) { return x * 2.0; })
            >> shell::output<double, kernel::clock_tag::ui>("out")
    };

    // Before the region has ticked, the output holds monostate: the strict
    // read throws, get_or substitutes the fallback.
    REQUIRE_THROWS_AS(kernel::get<double>(program, "out"), std::bad_variant_access);
    REQUIRE(program.get_or<double>("out", -1.0) == -1.0);

    program.set("in", 21.0);
    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(program.get_or<double>("out", -1.0) == 42.0);

    // A genuine type mismatch is still an error, never a silent fallback.
    REQUIRE_THROWS_AS(program.get_or<bool>("out", false), std::bad_variant_access);
}

TEST_CASE("shell/program | get_input reads back a set input", "[recipe]")
{
    kernel::program program
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("level")
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("level_out"),
            shell::input<std::optional<std::int64_t>, kernel::clock_tag::ui>("event")
                >> shell::output<std::optional<std::int64_t>, kernel::clock_tag::ui>("event_out"))
    };

    program.set("level", kernel::value { std::int64_t { 5 } });
    program.set("event", shell::to_value(std::optional<std::int64_t> { 7 }));

    REQUIRE(program.get_input("level") == kernel::value { std::int64_t { 5 } });
    REQUIRE(program.get_input("event") == kernel::value { std::int64_t { 7 } });

    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);

    // A level port persists; an event port is reverted after the tick that
    // consumes it, so reading it back tells a connector its event is done.
    REQUIRE(program.get_input("level") == kernel::value { std::int64_t { 5 } });
    REQUIRE(program.get_input("event") == kernel::value { std::monostate {} });

    // Typed and positional read-back mirror set/get.
    REQUIRE(kernel::get_input<std::int64_t>(program, "level") == 5);
    REQUIRE(program.get_input(0) == kernel::value { std::int64_t { 5 } });

    REQUIRE_THROWS_AS(program.get_input("nope"), std::runtime_error);
    REQUIRE_THROWS_AS(program.get_input(99), std::runtime_error);
}

TEST_CASE("shell/program | Unknown port names are rejected", "[D2][recipe]")
{
    kernel::program program
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("a") >> shell::output<std::int64_t, kernel::clock_tag::ui>("x")
    };

    REQUIRE_THROWS_AS(program.set("nope", 1), std::runtime_error);
}

TEST_CASE("shell/program | Owning and borrowing constructions of a delay agree", "[recipe]")
{ // The parity invariant: the same network built
  // from a recipe and from a compiled descriptor evaluates identically —
  // including the delay seed on the first tick and the shift on the second.
  // `static`: compile stores `self` pointers into the recipe, so the recipe
  // needs a constant address — and the borrower needs it to outlive the test.
    static constexpr auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
        >> shell::delay<std::int64_t>(0)
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    // MSVC cannot constant-evaluate the vector-based walk (see
    // descriptor_storage.test.cpp); compile at runtime there. The parity check
    // below is the point of the test, not the constant evaluation itself.
#if defined(_MSC_VER)
    static const auto compiled { shell::compile(network) };
#else
    static constexpr auto compiled { shell::compile(network) };
#endif

    kernel::program owned { network };
    kernel::program borrowed { compiled.descriptor() };

    kernel::release_engine engine;

    owned.set("in", 5);
    borrowed.set("in", 5);
    engine.step(owned, kernel::clock_tag::ui);
    engine.step(borrowed, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(owned, "out") == 0);
    REQUIRE(get<std::int64_t>(borrowed, "out") == 0);

    owned.set("in", 7);
    borrowed.set("in", 7);
    engine.step(owned, kernel::clock_tag::ui);
    engine.step(borrowed, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(owned, "out") == 5);
    REQUIRE(get<std::int64_t>(borrowed, "out") == 5);
}

TEST_CASE("shell/program | Duplicate port names are a build-time error", "[D2][recipe]")
{
    // Pins the obvious uniqueness rule — silent ambiguity is not an option.
    const auto chain_a { shell::input<std::int64_t, kernel::clock_tag::ui>("dup") >> shell::output<std::int64_t, kernel::clock_tag::ui>("x") };
    const auto chain_b { shell::input<std::int64_t, kernel::clock_tag::ui>("dup") >> shell::output<std::int64_t, kernel::clock_tag::ui>("y") };
    const auto network { parallel(chain_a, chain_b) };

    REQUIRE_THROWS_AS(kernel::program(network), std::runtime_error);
}

TEST_CASE("shell/program | Owning and borrowing non-trivial programs evaluate to the same result", "[constexpr]")
{
    // The hand-written-descriptor parity pin: the non-trivial network below
    // (share, zip, scan state, labels) subsumes the passthrough smoke that
    // used to stand here.
    using namespace clockr;

    const auto source { shell::share(shell::input<std::int64_t, kernel::clock_tag::ui>("in")) };
    const auto network
    {
        shell::zip(
            source
                >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x * 2; }),
            source
                >> shell::scan<std::int64_t, std::int64_t>(
                       20,
                       [](const std::int64_t& prev, const std::int64_t& in) { return prev + in; }
                   )
                >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 10; })
        )
        >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
               [](const std::int64_t& a, const std::int64_t& b) { return a + b; }
           )
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };
    kernel::program runtime_program { network };


    static constexpr auto compiled_passthrough
    {
        [](const void*, const kernel::tick_context&, std::span<const kernel::value> inputs)
        {
            return inputs.empty()
                ? kernel::value { std::monostate {} }
                : inputs.front();
        }
    };
    static constexpr std::array<const kernel::node_descriptor, 7> nodes
    {
        kernel::node_descriptor
        { // input
            0,
            kernel::clock_tag::ui,
            kernel::node_kind::input,
            compiled_passthrough,
            nullptr
        },
        kernel::node_descriptor
        { // branch1/lift
            1,
            kernel::clock_tag::ui,
            kernel::node_kind::internal,
            [](const void*, const kernel::tick_context&, std::span<const kernel::value> inputs)
            {
                return shell::to_value(shell::from_value<std::int64_t>(inputs.front()) * 2);
            },
            nullptr
        },
        kernel::node_descriptor
        { // branch2/delay
            2,
            kernel::clock_tag::ui,
            kernel::node_kind::delay,
            [](const void*, const kernel::tick_context&, std::span<const kernel::value>)
            {
                return shell::to_value(20);
            },
            nullptr
        },
        kernel::node_descriptor
        { // branch2/scanner
            3,
            kernel::clock_tag::ui,
            kernel::node_kind::internal,
            [](const void*, const kernel::tick_context&, std::span<const kernel::value> inputs)
            {
                return shell::to_value(
                    shell::from_value<std::int64_t>(inputs[0])    // delay
                    + shell::from_value<std::int64_t>(inputs[1])  // input
                );
            },
            nullptr
        },
        { // branch2/lift
            4,
            kernel::clock_tag::ui,
            kernel::node_kind::internal,
            [](const void*, const kernel::tick_context&, std::span<const kernel::value> inputs)
            {
                return shell::to_value(shell::from_value<std::int64_t>(inputs.front()) + 10);
            },
            nullptr
        },
        { // lift
            5,
            kernel::clock_tag::ui,
            kernel::node_kind::internal,
            [](const void*, const kernel::tick_context&, std::span<const kernel::value> inputs)
            {
                return shell::to_value(
                    shell::from_value<std::int64_t>(inputs[0])
                    + shell::from_value<std::int64_t>(inputs[1])
                );
            },
            nullptr
        },
        { // output
            6,
            kernel::clock_tag::ui,
            kernel::node_kind::output,
            compiled_passthrough,
            nullptr
        }
    };
    static constexpr std::array<const kernel::edge, 8> edges
    {
        kernel::edge { 0, 1 }, // input           -> branch1/lift
        kernel::edge { 2, 3 }, // branch2/delay   -> branch2/scanner
        kernel::edge { 0, 3 }, // input           -> branch2/scanner
        kernel::edge { 3, 2 }, // branch2/scanner -> branch2/delay
        kernel::edge { 3, 4 }, // branch2/scanner -> branch2/lift
        kernel::edge { 1, 5 }, // branch1/lift    -> lift
        kernel::edge { 4, 5 }, // branch2/lift    -> lift
        kernel::edge { 5, 6 }, // lift           -> output
    };
    static constexpr std::array<const kernel::node_id, 8> fan_in
    {
        0,    // [input] -> branch1/lift
        3,    // [branch2/scanner] -> branch2/delay
        2, 0, // [branch2_delay, input] -> branch2/scanner
        3,    // [branch2/scanner] -> branch2/lift
        1, 4, // [branch1/lift, branch2/lift] -> lift
        5,    // [lift] -> output
    };
    static constexpr std::array<const std::size_t, 8> fan_in_offsets
    {
        0, // input
        0, // branch1/lift
        1, // branch2/delay
        2, // branch2/scanner
        4, // branch2/lift
        5, // lift
        7, // output
        8, // end
    };
    static constexpr std::array<const kernel::node_id, 7> evaluation_order { 0, 1, 2, 3, 4, 5, 6 };
    static constexpr std::array<const kernel::region, 0> regions;
    static constexpr std::array<const kernel::crossing, 0> crossings;
    static constexpr std::array<const kernel::node_id, 1> input_ids { 0 };
    static constexpr std::array<const kernel::node_id, 1> output_ids { 6 };
    static constexpr std::array<const char*, 1> input_names { "in" };
    static constexpr std::array<const char*, 1> output_names { "out" };
    static constexpr std::array<const kernel::node_label, 7> labels
    {
        kernel::node_label { "input", "in" },
        kernel::node_label { "lift", nullptr },
        kernel::node_label { "scan", nullptr },
        kernel::node_label { "scan", nullptr },
        kernel::node_label { "lift", nullptr },
        kernel::node_label { "lift", nullptr },
        kernel::node_label { "output", "out" },
    };
    static constexpr kernel::descriptor_view descriptor
    {
        nodes,
        labels,
        edges,
        fan_in,
        fan_in_offsets,
        evaluation_order,
        regions,
        crossings,
        input_ids,
        output_ids,
        input_names,
        output_names,
    };

    kernel::program static_program
    {
        descriptor,
    };

    runtime_program.set("in", 23);
    static_program.set("in", 23);

    kernel::release_engine engine;

    // branch1(23 * 2) + branch2((20 + 23) + 10) = 99
    engine.step(runtime_program, kernel::clock_tag::ui);
    engine.step(static_program, kernel::clock_tag::ui);
    REQUIRE(shell::from_value<std::int64_t>(static_program.get(0)) == 99);
    REQUIRE(get<std::int64_t>(static_program, "out") == get<std::int64_t>(runtime_program, "out"));

    // branch1(23 * 2) + branch2((43 + 23) + 10) = 122
    engine.step(runtime_program, kernel::clock_tag::ui);
    engine.step(static_program, kernel::clock_tag::ui);
    REQUIRE(shell::from_value<std::int64_t>(static_program.get(0)) == 122);
    REQUIRE(get<std::int64_t>(static_program, "out") == get<std::int64_t>(runtime_program, "out"));

    runtime_program.set("in", -1);
    static_program.set(0, -1);

    // branch1(-1 * 2) + branch2((66 + -1) + 10) = 73
    engine.step(runtime_program, kernel::clock_tag::ui);
    engine.step(static_program, kernel::clock_tag::ui);
    REQUIRE(shell::from_value<std::int64_t>(static_program.get(0)) == 73);
    REQUIRE(get<std::int64_t>(static_program, "out") == get<std::int64_t>(runtime_program, "out"));
}

TEST_CASE("shell/program | Lowered nodes carry operator labels", "[recipe]")
{
    // Every graph-emitting operator stamps a label on the node(s) it lowers
    // to, so tools can name a node instead of showing a bare id. Structural
    // operators (compose/zip/parallel/share) emit nothing of their own.
    const auto network
    {
        shell::zip(
            shell::input<std::int64_t, kernel::clock_tag::ui>("a"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("b"))
            >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
                   [](const std::int64_t& x, const std::int64_t& y) { return x + y; })
            >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& v) { return v * 2; })
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };
    const auto graph { program.descriptor() };

    REQUIRE(graph.labels.size() == graph.nodes.size());

    // Labels follow node ids (which are dense and declaration-ordered here).
    REQUIRE(std::string_view { kernel::label(graph, 0).combinator } == "input");
    REQUIRE(std::string_view { kernel::label(graph, 1).combinator } == "input");
    REQUIRE(std::string_view { kernel::label(graph, 2).combinator } == "lift");
    REQUIRE(std::string_view { kernel::label(graph, 3).combinator } == "lift");
    REQUIRE(std::string_view { kernel::label(graph, 4).combinator } == "output");

    // Boundary ports name themselves; interior nodes carry no port name.
    REQUIRE(std::string_view { kernel::label(graph, 0).port_name } == "a");
    REQUIRE(std::string_view { kernel::label(graph, 1).port_name } == "b");
    REQUIRE(kernel::label(graph, 2).port_name == nullptr);
    REQUIRE(std::string_view { kernel::label(graph, 4).port_name } == "out");
}

TEST_CASE("shell/program | Labels agree on the owning and borrowing paths", "[recipe]")
{
    // The recipe walk and the compiled descriptor are two encodings of the
    // same lowering; their labels must match (the label analogue of the
    // owning/borrowing evaluation parity in the tests above).
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
            >> shell::scan<std::int64_t, std::int64_t>(
                   0,
                   [](const std::int64_t& previous, const std::int64_t& v) { return previous + v; })
            >> shell::output<std::int64_t, kernel::clock_tag::ui>("out")
    };

    kernel::program owning { network };
    kernel::program borrowing { shell::compile(network).descriptor() };

    const auto owning_graph { owning.descriptor() };
    const auto borrowing_graph { borrowing.descriptor() };

    REQUIRE(owning_graph.labels.size() == borrowing_graph.labels.size());

    for (kernel::node_id id { 0 }; id < owning_graph.labels.size(); ++id)
    {
        const auto a { kernel::label(owning_graph, id) };
        const auto b { kernel::label(borrowing_graph, id) };

        REQUIRE((a.combinator == nullptr) == (b.combinator == nullptr));

        if (a.combinator && b.combinator)
            REQUIRE(std::string_view { a.combinator } == std::string_view { b.combinator });

        REQUIRE((a.port_name == nullptr) == (b.port_name == nullptr));

        if (a.port_name && b.port_name)
            REQUIRE(std::string_view { a.port_name } == std::string_view { b.port_name });
    }
}
