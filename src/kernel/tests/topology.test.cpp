#include "clockr/shell/recipe.hpp"
#include "clockr/shell/builder.hpp"
#include "clockr/shell/draft.hpp"

#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/topology.hpp"
#include "clockr/kernel/descriptor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace
{
    using namespace clockr;
    using namespace clockr::kernel;

    node::tick_fn
    pass_through();

    node::tick_fn
    sum_of();

    node::tick_fn
    increment();
} // namespace

namespace
{
    // Plain-data probes: networks described as node_descriptor arrays assembled
    // into descriptor descriptors — the same types the engine consumes —
    // with no std::function anywhere, so every probe is a literal value and
    // the whole ordering/region analysis runs during constant evaluation. The
    // structural-graph twins in the test cases below mirror them node for node.
    constexpr node_descriptor::tick_fn probe_tick
    {
        [](const void*, const tick_context&, std::span<const value>) { return value {}; }
    };

    constexpr node_descriptor
    probe(node_id id, clock_tag clock, node_kind kind)
    {
        return node_descriptor { id, clock, kind, probe_tick, nullptr };
    }

    // Shared empty tables: probes describe structure only, so every port
    // table and precomputed view stays empty.
    constexpr std::array<const node_id, 0> no_ids {};
    constexpr std::array<const region, 0> no_regions {};
    constexpr std::array<const crossing, 0> no_crossings {};
    constexpr std::array<const char* const, 0> no_names {};
    constexpr std::array<const node_label, 0> no_labels {};

    // 0 -> 1 -> 2 -> 3, one thread.
    constexpr std::array<const node_descriptor, 4> chain_nodes
    {
        probe(0, clock_tag::audio, node_kind::input),
        probe(1, clock_tag::audio, node_kind::internal),
        probe(2, clock_tag::audio, node_kind::internal),
        probe(3, clock_tag::audio, node_kind::output)
    };
    constexpr std::array<const edge, 3> chain_edges
    {
        edge { 0, 1 }, edge { 1, 2 }, edge { 2, 3 }
    };

    // 0 fans out to {1, 2}, both join at 3.
    constexpr std::array<const node_descriptor, 4> diamond_nodes
    {
        probe(0, clock_tag::audio, node_kind::input),
        probe(1, clock_tag::audio, node_kind::internal),
        probe(2, clock_tag::audio, node_kind::internal),
        probe(3, clock_tag::audio, node_kind::output)
    };
    constexpr std::array<const edge, 4> diamond_edges
    {
        edge { 0, 1 }, edge { 0, 2 }, edge { 1, 3 }, edge { 2, 3 }
    };

    // Same shape as diamond, edges listed in reverse discovery order.
    constexpr std::array<const node_descriptor, 4> shuffled_diamond_nodes
    {
        probe(0, clock_tag::audio, node_kind::input),
        probe(1, clock_tag::audio, node_kind::internal),
        probe(2, clock_tag::audio, node_kind::internal),
        probe(3, clock_tag::audio, node_kind::output)
    };
    constexpr std::array<const edge, 4> shuffled_diamond_edges
    {
        edge { 2, 3 }, edge { 1, 3 }, edge { 0, 2 }, edge { 0, 1 }
    };

    // Two independent chains whose roots become ready simultaneously: the
    // tie must break towards the lowest ready node id.
    constexpr std::array<const node_descriptor, 4> forks_nodes
    {
        probe(0, clock_tag::audio, node_kind::internal),
        probe(1, clock_tag::audio, node_kind::internal),
        probe(2, clock_tag::audio, node_kind::internal),
        probe(3, clock_tag::audio, node_kind::internal)
    };
    constexpr std::array<const edge, 2> forks_edges
    {
        edge { 0, 2 }, edge { 1, 3 }
    };

    // A delay-free cycle: rejected at runtime (throw), and it cannot pass
    // constant evaluation — there is deliberately no static_assert over this
    // probe, because failing to compile *is* its compile-time contract.
    // Chunk G turns that into a readable validity check.
    constexpr std::array<const node_descriptor, 2> cycle_nodes
    {
        probe(0, clock_tag::ui, node_kind::internal),
        probe(1, clock_tag::ui, node_kind::internal)
    };
    constexpr std::array<const edge, 2> cycle_edges
    {
        edge { 0, 1 }, edge { 1, 0 }
    };

    // A cycle guarded by a delay: legal (L7).
    constexpr std::array<const node_descriptor, 2> guarded_cycle_nodes
    {
        probe(0, clock_tag::ui, node_kind::delay),
        probe(1, clock_tag::ui, node_kind::internal)
    };
    constexpr std::array<const edge, 2> guarded_cycle_edges
    {
        edge { 0, 1 }, edge { 1, 0 }
    };

    // Feedback into a delay from a computed value.
    constexpr std::array<const node_descriptor, 3> feedback_nodes
    {
        probe(0, clock_tag::audio, node_kind::input),
        probe(1, clock_tag::audio, node_kind::delay),
        probe(2, clock_tag::audio, node_kind::internal)
    };
    constexpr std::array<const edge, 3> feedback_edges
    {
        edge { 0, 1 }, edge { 1, 2 }, edge { 2, 1 }
    };

    // audio -> ui -> audio: three regions, two crossings.
    constexpr std::array<const node_descriptor, 3> mixed_nodes
    {
        probe(0, clock_tag::audio, node_kind::input),
        probe(1, clock_tag::ui, node_kind::internal),
        probe(2, clock_tag::audio, node_kind::output)
    };
    constexpr std::array<const edge, 2> mixed_edges
    {
        edge { 0, 1 }, edge { 1, 2 }
    };

    // Same-thread subgraphs joined through a foreign thread stay separate.
    constexpr std::array<const node_descriptor, 4> hub_nodes
    {
        probe(0, clock_tag::audio, node_kind::input),
        probe(1, clock_tag::audio, node_kind::input),
        probe(2, clock_tag::worker, node_kind::internal),
        probe(3, clock_tag::audio, node_kind::output)
    };
    constexpr std::array<const edge, 3> hub_edges
    {
        edge { 0, 2 }, edge { 1, 2 }, edge { 2, 3 }
    };

    // Descriptor assembly: nodes and edges carry all the information the
    // ordering and region analyses read; the remaining views stay empty, and
    // the fan-in prefix sums stay all-zero so fan_in(id) is well-formed but
    // empty for every node.
    constexpr std::array<const std::size_t, 5> chain_fan_in_offsets {};
    constexpr std::array<const std::size_t, 5> diamond_fan_in_offsets {};
    constexpr std::array<const std::size_t, 5> forks_fan_in_offsets {};
    constexpr std::array<const std::size_t, 5> guarded_cycle_fan_in_offsets {};
    constexpr std::array<const std::size_t, 5> feedback_fan_in_offsets {};
    constexpr std::array<const std::size_t, 5> mixed_fan_in_offsets {};
    constexpr std::array<const std::size_t, 5> hub_fan_in_offsets {};

    constexpr descriptor_view chain_probe
    {
        .nodes           = chain_nodes,
        .labels          = no_labels,
        .edges           = chain_edges,
        .fan_in          = no_ids,
        .fan_in_offsets  = chain_fan_in_offsets,
        .evaluation_order = no_ids,
        .regions         = no_regions,
        .crossings  = no_crossings,
        .input_ids       = no_ids,
        .output_ids      = no_ids,
        .input_names     = no_names,
        .output_names    = no_names
    };
    constexpr shell::flat_map<node_id, conc::policy> chain_probe_crossings;

    constexpr descriptor_view diamond_probe
    {
        .nodes           = diamond_nodes,
        .labels          = no_labels,
        .edges           = diamond_edges,
        .fan_in          = no_ids,
        .fan_in_offsets  = diamond_fan_in_offsets,
        .evaluation_order = no_ids,
        .regions         = no_regions,
        .crossings  = no_crossings,
        .input_ids       = no_ids,
        .output_ids      = no_ids,
        .input_names     = no_names,
        .output_names    = no_names
    };

    constexpr descriptor_view forks_probe
    {
        .nodes           = forks_nodes,
        .labels          = no_labels,
        .edges           = forks_edges,
        .fan_in          = no_ids,
        .fan_in_offsets  = forks_fan_in_offsets,
        .evaluation_order = no_ids,
        .regions         = no_regions,
        .crossings  = no_crossings,
        .input_ids       = no_ids,
        .output_ids      = no_ids,
        .input_names     = no_names,
        .output_names    = no_names
    };

    constexpr descriptor_view guarded_cycle_probe
    {
        .nodes           = guarded_cycle_nodes,
        .labels          = no_labels,
        .edges           = guarded_cycle_edges,
        .fan_in          = no_ids,
        .fan_in_offsets  = guarded_cycle_fan_in_offsets,
        .evaluation_order = no_ids,
        .regions         = no_regions,
        .crossings  = no_crossings,
        .input_ids       = no_ids,
        .output_ids      = no_ids,
        .input_names     = no_names,
        .output_names    = no_names
    };

    constexpr descriptor_view feedback_probe
    {
        .nodes           = feedback_nodes,
        .labels          = no_labels,
        .edges           = feedback_edges,
        .fan_in          = no_ids,
        .fan_in_offsets  = feedback_fan_in_offsets,
        .evaluation_order = no_ids,
        .regions         = no_regions,
        .crossings  = no_crossings,
        .input_ids       = no_ids,
        .output_ids      = no_ids,
        .input_names     = no_names,
        .output_names    = no_names
    };

    constexpr descriptor_view mixed_probe
    {
        .nodes           = mixed_nodes,
        .labels          = no_labels,
        .edges           = mixed_edges,
        .fan_in          = no_ids,
        .fan_in_offsets  = mixed_fan_in_offsets,
        .evaluation_order = no_ids,
        .regions         = no_regions,
        .crossings  = no_crossings,
        .input_ids       = no_ids,
        .output_ids      = no_ids,
        .input_names     = no_names,
        .output_names    = no_names
    };

    constexpr descriptor_view hub_probe
    {
        .nodes           = hub_nodes,
        .labels          = no_labels,
        .edges           = hub_edges,
        .fan_in          = no_ids,
        .fan_in_offsets  = hub_fan_in_offsets,
        .evaluation_order = no_ids,
        .regions         = no_regions,
        .crossings  = no_crossings,
        .input_ids       = no_ids,
        .output_ids      = no_ids,
        .input_names     = no_names,
        .output_names    = no_names
    };

    // --- compile-time guarantees ---------------------------------------------------

    static_assert(
        evaluation_order<const node_descriptor>(chain_nodes, chain_edges)
            == std::vector<node_id> { 0, 1, 2, 3 },
        "a linear chain orders strictly upstream to downstream");

    static_assert(
        evaluation_order<const node_descriptor>(diamond_nodes, diamond_edges)
            == std::vector<node_id> { 0, 1, 2, 3 },
        "a diamond evaluates both branches before the join");

    static_assert(
        evaluation_order<const node_descriptor>(shuffled_diamond_nodes, shuffled_diamond_edges)
            == evaluation_order<const node_descriptor>(diamond_nodes, diamond_edges),
        "the order depends on the graph, not on the edge insertion order");

    static_assert(
        evaluation_order<const node_descriptor>(forks_nodes, forks_edges)
            == std::vector<node_id> { 0, 1, 2, 3 },
        "ties break towards the lowest ready node id — the determinism policy");

    static_assert(
        evaluation_order<const node_descriptor>(guarded_cycle_nodes, guarded_cycle_edges)
            == std::vector<node_id> { 0, 1 },
        "a delay is a Kahn seed: the guarded cycle passes constant evaluation");

    static_assert(
        evaluation_order<const node_descriptor>(feedback_nodes, feedback_edges)
            == std::vector<node_id> { 0, 1, 2 },
        "the delay settles before its own feedback source is evaluated");

    static_assert(
        []
        {
            const auto regions = process_regions(chain_probe, 4, chain_probe_crossings);

            return regions.regions.size() == 1
                && regions.crossings.empty()
                && regions.region_of == std::vector<std::size_t> { 0, 0, 0, 0 }
                && regions.regions[0].clock == clock_tag::audio
                && regions.regions[0].nodes == std::vector<node_id> { 0, 1, 2, 3 };
        }(),
        "a single-thread graph is one region with no crossings");

} // namespace

TEST_CASE("kernel/topology | Cross-thread hop splits regions and records crossings", "[kernel]")
{
    shell::flat_map<node_id, conc::policy> crossings;
    crossings.try_emplace(0, conc::policy::latest);
    crossings.try_emplace(1, conc::policy::latest);

    const auto regions = process_regions(mixed_probe, 3, crossings);

    REQUIRE(regions.regions.size() == 3);
    REQUIRE(regions.crossings.size() == 2);
    REQUIRE(regions.region_of == std::vector<std::size_t> { 0, 1, 2 });
    REQUIRE(regions.regions[0].clock == clock_tag::audio);
    REQUIRE(regions.regions[1].clock == clock_tag::ui);
    REQUIRE(regions.regions[2].clock == clock_tag::audio);
    REQUIRE(regions.regions[0].nodes == std::vector<node_id> { 0 });
    REQUIRE(regions.regions[1].nodes == std::vector<node_id> { 1 });
    REQUIRE(regions.regions[2].nodes == std::vector<node_id> { 2 });
    REQUIRE(regions.crossings[0].from == 0);
    REQUIRE(regions.crossings[0].to == 1);
    REQUIRE(regions.crossings[1].from == 1);
    REQUIRE(regions.crossings[1].to == 2);
}

TEST_CASE("kernel/topology | Same-thread subgraphs joined through another thread stay separate regions (runtime)", "[kernel]")
{
    shell::flat_map<node_id, conc::policy> crossings;
    crossings.try_emplace(0, conc::policy::latest);
    crossings.try_emplace(1, conc::policy::latest);
    crossings.try_emplace(2, conc::policy::latest);

    const auto regions = process_regions(hub_probe, 4, crossings);

    REQUIRE(regions.regions.size() == 4);
    REQUIRE(regions.crossings.size() == 3);
    REQUIRE(regions.region_of == std::vector<std::size_t> { 0, 1, 2, 3 });
}

TEST_CASE("kernel/topology | Evaluation order respects every dependency", "[kernel]")
{
    using namespace clockr::kernel;

    clockr::kernel::graph g;

    const auto a { g.add_node(clock_tag::audio, node_kind::input, pass_through()) };
    const auto b { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
    const auto c { g.add_node(clock_tag::ui, node_kind::internal, pass_through()) };
    const auto d { g.add_node(clock_tag::audio, node_kind::output, pass_through()) };

    g.connect(a, b);
    g.connect(b, c);
    g.connect(c, d);

    shell::flat_map<node_id, conc::policy> crossings;
    crossings.try_emplace(b, conc::policy::latest);
    crossings.try_emplace(c, conc::policy::latest);

    const topology t { g, crossings };

    const auto& order { t.evaluation_order };

    REQUIRE(order.size() == 4);

    std::size_t pos_a { 0 };
    std::size_t pos_b { 0 };
    std::size_t pos_c { 0 };
    std::size_t pos_d { 0 };

    for (std::size_t i { 0 }; i < order.size(); ++i)
    {
        if (order[i] == a)
            pos_a = i;
        else if (order[i] == b)
            pos_b = i;
        else if (order[i] == c)
            pos_c = i;
        else if (order[i] == d)
            pos_d = i;
    }

    REQUIRE(pos_a < pos_b);
    REQUIRE(pos_b < pos_c);
    REQUIRE(pos_c < pos_d);
}

TEST_CASE("kernel/topology | A delay-free cyclical graph is rejected", "[A1][kernel]")
{ // L7: cycles are legal iff every cycle crosses at least one delay node.
  // A delay-free cycle deadlocks Kahn's algorithm → rejected at build time.
    using namespace clockr::kernel;

    clockr::kernel::graph g;

    const auto a { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
    const auto b { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };

    g.connect(a, b);
    g.connect(b, a);

    constexpr shell::flat_map<node_id, conc::policy> crossings;

    auto make_topology = [&] { return topology { g, crossings }; };
    REQUIRE_THROWS_AS(make_topology(), std::runtime_error);
}

TEST_CASE("kernel/topology | A cycle through a delay is accepted", "[A1][kernel]")
{ // L7's positive case: the delay is a Kahn seed, so the feedback edge never
  // creates an in-degree cycle; the graph is well-defined.
    using namespace clockr::kernel;

    clockr::kernel::graph g;

    const auto d { g.add_delay(clock_tag::ui, value { std::int64_t { 0 } }) };
    const auto inc { g.add_node(clock_tag::ui, node_kind::internal, increment()) };

    g.connect(d, inc);
    g.connect(inc, d); // feedback, guarded by the delay

    constexpr shell::flat_map<node_id, conc::policy> crossings;

    REQUIRE_NOTHROW(topology { g, crossings });
}

TEST_CASE("kernel/topology | Plain-data and structural-graph descriptions agree on evaluation order", "[kernel][constexpr]")
{ // The two-plane parity invariant, topology slice: the same shape described
  // as constant data and as a runtime-built graph orders identically. The
  // compile-time half of each comparison is pinned by the static_asserts
  // above; this runs the identical analysis over both descriptions at
  // runtime and compares the full struct.
    using namespace clockr::kernel;

    const auto check = [](const clockr::kernel::graph& dev, const auto& probe, shell::flat_map<node_id, conc::policy> crossings)
    {
        const topology dev_topology { dev, crossings };
        const topology probe_topology { probe, crossings };

        REQUIRE(dev_topology.evaluation_order == probe_topology.evaluation_order);
    };

    { // chain_probe
        clockr::kernel::graph g;

        const auto a { g.add_node(clock_tag::audio, node_kind::input, pass_through()) };
        const auto b { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
        const auto c { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
        const auto d { g.add_node(clock_tag::audio, node_kind::output, pass_through()) };

        g.connect(a, b);
        g.connect(b, c);
        g.connect(c, d);

        constexpr shell::flat_map<node_id, conc::policy> crossings;

        check(g, chain_probe, crossings);
    }

    { // diamond_probe
        clockr::kernel::graph g;

        const auto a { g.add_node(clock_tag::audio, node_kind::input, pass_through()) };
        const auto b { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
        const auto c { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
        const auto d { g.add_node(clock_tag::audio, node_kind::output, pass_through()) };

        g.connect(a, b);
        g.connect(a, c);
        g.connect(b, d);
        g.connect(c, d);

        constexpr shell::flat_map<node_id, conc::policy> crossings;

        check(g, diamond_probe, crossings);
    }

    { // forks_probe: two roots ready at once — pins the tie-break policy
      // against the structural-graph path too.
        clockr::kernel::graph g;

        const auto a { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
        const auto b { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
        const auto c { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };
        const auto d { g.add_node(clock_tag::audio, node_kind::internal, pass_through()) };

        g.connect(a, c);
        g.connect(b, d);

        constexpr shell::flat_map<node_id, conc::policy> crossings;

        check(g, forks_probe, crossings);
    }

    { // guarded_cycle_probe
        clockr::kernel::graph g;

        const auto d { g.add_delay(clock_tag::ui, value { std::int64_t { 0 } }) };
        const auto inc { g.add_node(clock_tag::ui, node_kind::internal, increment()) };

        g.connect(d, inc);
        g.connect(inc, d);

        constexpr shell::flat_map<node_id, conc::policy> crossings;

        check(g, guarded_cycle_probe, crossings);
    }

    { // feedback_probe
        clockr::kernel::graph g;

        const auto in { g.add_node(clock_tag::audio, node_kind::input, pass_through()) };
        const auto d { g.add_delay(clock_tag::audio, value { std::int64_t { 0 } }) };
        const auto scan { g.add_node(clock_tag::audio, node_kind::internal, increment()) };

        g.connect(in, d);
        g.connect(d, scan);
        g.connect(scan, d);

        constexpr shell::flat_map<node_id, conc::policy> crossings;

        check(g, feedback_probe, crossings);
    }
}

TEST_CASE("kernel/topology | A delay-free cycle fails at runtime over plain data too", "[kernel][constexpr]")
{ // The compile-time contract of the cycle probe is that constant evaluation
  // fails (there is no static_assert to write); the runtime contract is the
  // familiar throw, exercised here over the very same plain-data probe.
    using namespace clockr::kernel;

    REQUIRE_THROWS_AS(
        evaluation_order<const node_descriptor>(cycle_nodes, cycle_edges),
        std::runtime_error
    );
}

TEST_CASE("kernel/topology | Plain-data and structural-graph descriptions agree on regions", "[kernel][constexpr]")
{ // Delta-free parity: regions, crossings and region membership come out
  // identical whichever description produced them.
    using namespace clockr::kernel;

    const auto check = [](const clockr::kernel::graph& dev, const auto& probe, const shell::flat_map<node_id, conc::policy>& crossings)
    {
        const topology t { dev, crossings };
        const auto processed { process_regions(probe, size(probe), crossings) };

        REQUIRE(t.regions.size() == processed.regions.size());
        REQUIRE(t.crossings.size() == processed.crossings.size());
        REQUIRE(t.region_of == processed.region_of);

        for (std::size_t i { 0 }; i < t.regions.size(); ++i)
        {
            REQUIRE(t.regions[i].clock == processed.regions[i].clock);
            REQUIRE(t.regions[i].nodes == processed.regions[i].nodes);
        }

        for (std::size_t i { 0 }; i < t.crossings.size(); ++i)
        {
            REQUIRE(t.crossings[i].from == processed.crossings[i].from);
            REQUIRE(t.crossings[i].to == processed.crossings[i].to);
        }
    };

    { // mixed_probe: audio -> ui -> audio
        clockr::kernel::graph g;

        const auto in { g.add_node(clock_tag::audio, node_kind::input, pass_through()) };
        const auto mid { g.add_node(clock_tag::ui, node_kind::internal, pass_through()) };
        const auto out { g.add_node(clock_tag::audio, node_kind::output, pass_through()) };

        g.connect(in, mid);
        g.connect(mid, out);

        shell::flat_map<node_id, conc::policy> crossings;
        crossings.try_emplace(in, conc::policy::latest);
        crossings.try_emplace(mid, conc::policy::latest);

        check(g, mixed_probe, crossings);
    }

    { // hub_probe: two audio inputs joined through a worker hub
        clockr::kernel::graph g;

        const auto first { g.add_node(clock_tag::audio, node_kind::input, pass_through()) };
        const auto second { g.add_node(clock_tag::audio, node_kind::input, pass_through()) };
        const auto hub { g.add_node(clock_tag::worker, node_kind::internal, sum_of()) };
        const auto out { g.add_node(clock_tag::audio, node_kind::output, pass_through()) };

        g.connect(first, hub);
        g.connect(second, hub);
        g.connect(hub, out);

        shell::flat_map<node_id, conc::policy> crossings;
        crossings.try_emplace(first, conc::policy::latest);
        crossings.try_emplace(second, conc::policy::latest);
        crossings.try_emplace(hub, conc::policy::latest);

        check(g, hub_probe, crossings);
    }
}

namespace
{
    using namespace clockr;

    node::tick_fn
    pass_through()
    {
        return [](const tick_context&, std::span<const value> inputs)
        {
            return inputs.front();
        };
    }

    node::tick_fn
    sum_of()
    {
        return [](const tick_context&, std::span<const value> inputs)
        {
            std::int64_t total { 0 };

            for (const auto& v : inputs)
                total += std::get<std::int64_t>(v);

            return value { total };
        };
    }

    node::tick_fn
    increment()
    {
        return [](const tick_context&, std::span<const value> inputs)
          {
              return value { std::get<std::int64_t>(inputs.front()) + 1 };
        };
    }
} // namespace
