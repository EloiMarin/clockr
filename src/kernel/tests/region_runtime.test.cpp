// region_runtime slot tables: the id -> slot index the tick path trusts.
//
// These tests pin the RT-hardened lookup contract: every port/delay node of a
// region resolves through its table, nothing else does, and named/positional
// addressing agrees with the tables end to end (set -> tick -> get). The old
// positional-counter engine passed the same behaviors by coincidence of visit
// order; these tests fail if the tables ever drift from the id lists, while a
// behavioral test alone could not tell.

#include "clockr/kernel/program.hpp"

#include "clockr/kernel/release_engine.hpp"

#include "clockr/shell/recipe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace clockr;

namespace
{

constexpr std::size_t no_slot { kernel::program::region_runtime::no_slot };

void
check_tables_cover_lists(const kernel::program::region_runtime& runtime, std::size_t node_count)
{
    REQUIRE(runtime.input_slot_of.size() == node_count);
    REQUIRE(runtime.output_slot_of.size() == node_count);
    REQUIRE(runtime.delay_slot_of.size() == node_count);

    for (std::size_t slot { 0 }; slot < runtime.input_ids.size(); ++slot)
        REQUIRE(runtime.input_slot_of[runtime.input_ids[slot]] == slot);

    for (std::size_t slot { 0 }; slot < runtime.output_ids.size(); ++slot)
        REQUIRE(runtime.output_slot_of[runtime.output_ids[slot]] == slot);

    for (std::size_t slot { 0 }; slot < runtime.delay_ids.size(); ++slot)
        REQUIRE(runtime.delay_slot_of[runtime.delay_ids[slot]] == slot);
}

void
check_tables_cover_only_lists(const kernel::program::region_runtime& runtime, std::size_t node_count)
{
    const auto listed = [&](kernel::node_id id, const std::vector<kernel::node_id>& ids)
    {
        return std::ranges::find(ids, id) != std::end(ids);
    };

    for (kernel::node_id id { 0 }; id < node_count; ++id)
    {
        if (!listed(id, runtime.input_ids))
            REQUIRE(runtime.input_slot_of[id] == no_slot);

        if (!listed(id, runtime.output_ids))
            REQUIRE(runtime.output_slot_of[id] == no_slot);

        if (!listed(id, runtime.delay_ids))
            REQUIRE(runtime.delay_slot_of[id] == no_slot);
    }
}

} // namespace

TEST_CASE("kernel/region | Slot tables map every listed id to its slot and nothing else", "[kernel][region]")
{
    // Three regions, ports on each, state + a crossing on audio: the table
    // fill must partition correctly on every clock at once.
    const auto network
    {
        shell::parallel(
            shell::at<kernel::clock_tag::audio, conc::policy::latest>(
                shell::input<double, kernel::clock_tag::ui>("bpm"))
                >> shell::scan<double, double>(0.0, [](const double& p, const double& b) { return p + b; })
                >> shell::output<double, kernel::clock_tag::audio>("pos"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("u")
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("uout"),
            shell::input<std::int64_t, kernel::clock_tag::worker>("w")
                >> shell::output<std::int64_t, kernel::clock_tag::worker>("wout")
        )
    };

    kernel::program program { network };

    const std::size_t node_count { nodes(program.descriptor()).size() };
    REQUIRE(node_count > 0);

    for (const auto tag :
         { kernel::clock_tag::audio, kernel::clock_tag::ui, kernel::clock_tag::worker })
    {
        const auto& runtime { program.region(tag) };

        check_tables_cover_lists(runtime, node_count);
        check_tables_cover_only_lists(runtime, node_count);

        // Evaluation order is this region's slice of the single global
        // linearization (L1): every node belongs here, none twice.
        for (const auto id : runtime.evaluation_order)
            REQUIRE(program.topology().regions[program.topology().region_of[id]].clock == tag);
    }

    // Port identity is by id, not by order: named and positional addressing
    // resolve to the same slots.
    REQUIRE(program.region(kernel::clock_tag::ui).input_ids.size() == 2);

    // The crossing registered link endpoints on both sides.
    const auto& audio { program.region(kernel::clock_tag::audio) };
    const auto& ui { program.region(kernel::clock_tag::ui) };
    REQUIRE(!audio.receives_latest.empty());
    REQUIRE(!ui.sends_latest.empty());
    REQUIRE(audio.delay_ids.size() == 1);
    REQUIRE(audio.delay_states.size() == 1);
}

TEST_CASE("kernel/region | Set-tick-get round-trips through the tables", "[kernel][region]")
{
    // Behavioral proof the engine reads what set() wrote: single-region
    // passthroughs on two clocks (multi-region chains need fill/drain, C2).
    const auto network
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("u")
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("uout"),
            shell::input<std::int64_t, kernel::clock_tag::worker>("w")
                >> shell::output<std::int64_t, kernel::clock_tag::worker>("wout")
        )
    };

    kernel::program program { network };
    kernel::release_engine engine;

    program.set("u", std::int64_t { 7 });
    program.set(1, std::int64_t { 9 }); // positional: second declared input
    engine.step(program, kernel::clock_tag::ui);
    engine.step(program, kernel::clock_tag::worker);

    REQUIRE(kernel::get<std::int64_t>(program, "uout") == 7);
    REQUIRE(kernel::get<std::int64_t>(program, "wout") == 9);
    REQUIRE(program.get(0) == kernel::value { std::int64_t { 7 } });
    REQUIRE(program.get(1) == kernel::value { std::int64_t { 9 } });
}

TEST_CASE("kernel/region | The compiled mirror builds identical table coverage", "[kernel][region]")
{
    // Same contract on the compiled plane (which derives regions from spans,
    // not from a draft): guards the second construct implementation, which
    // behavioral parity tests would only catch once ticked.
    static constexpr auto passthrough = [](const void*, const kernel::tick_context&, std::span<const kernel::value> inputs)
    {
        return inputs.empty() ? kernel::value { std::monostate {} } : inputs.front();
    };

    static constexpr std::array<const kernel::node_descriptor, 2> nodes
    {
        kernel::node_descriptor{ 0, kernel::clock_tag::ui, kernel::node_kind::input, passthrough, nullptr },
        kernel::node_descriptor{ 1, kernel::clock_tag::ui, kernel::node_kind::output, passthrough, nullptr },
    };
    static constexpr std::array<const kernel::edge, 1> edges { kernel::edge{ 0, 1 } };
    static constexpr std::array<const kernel::node_id, 1> fan_in { 0 };
    static constexpr std::array<const std::size_t, 3> fan_in_offsets { 0, 0, 1 };
    static constexpr std::array<const kernel::node_id, 2> order { 0, 1 };
    static constexpr std::array<const kernel::region, 0> regions;
    static constexpr std::array<const kernel::crossing, 0> crossings;
    static constexpr std::array<const kernel::node_id, 1> input_ids { 0 };
    static constexpr std::array<const kernel::node_id, 1> output_ids { 1 };
    static constexpr std::array<const char*, 1> input_names { "in" };
    static constexpr std::array<const char*, 1> output_names { "out" };
    static constexpr std::array<const kernel::node_label, 0> labels;
    static constexpr kernel::descriptor_view descriptor
    {
        nodes, labels, edges, fan_in, fan_in_offsets, order, regions, crossings,
        input_ids, output_ids, input_names, output_names,
    };

    kernel::program program { descriptor };

    const auto& runtime { program.region(kernel::clock_tag::ui) };

    check_tables_cover_lists(runtime, 2);
    check_tables_cover_only_lists(runtime, 2);

    program.set("in", std::int64_t { 41 });

    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);

    REQUIRE(kernel::get<std::int64_t>(program, "out") == 41);
    REQUIRE(program.get(0) == kernel::value { std::int64_t { 41 } });
}
