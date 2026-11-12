#include "clockr/replay/scenario.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>

using namespace clockr;
using namespace clockr::replay;

namespace
{

encoded_value
scalar(double value)
{
    encoded_value encoded;
    encoded.payload = value;
    return encoded;
}

} // namespace

TEST_CASE(
    "replay | Entry ids are monotonic and a step closes an epoch group",
    "[replay][scenario]"
)
{
    scenario_builder builder { run_id { 1 }, branch_id { 1 } };

    builder.mark_initialized(kernel::clock_tag::audio);
    builder.begin_epoch(kernel::clock_tag::ui);
    builder.set_input("a", scalar(1.0), false);
    builder.set_input("b", scalar(2.0), false);
    const auto step { builder.step_region(kernel::clock_tag::ui) };
    builder.begin_epoch(kernel::clock_tag::ui);
    builder.set_input("a", scalar(3.0), false);
    builder.step_region(kernel::clock_tag::ui);

    const auto scenario { builder.finish() };

    REQUIRE(scenario.entries.size() == 6);
    REQUIRE(scenario.entries[0].id == entry_id { 1 });
    REQUIRE(scenario.entries[1].id == entry_id { 2 });
    REQUIRE(scenario.entries[2].id == entry_id { 3 });
    REQUIRE(scenario.entries[3].id == entry_id { 4 });
    REQUIRE(scenario.entries[4].id == entry_id { 5 });
    REQUIRE(scenario.entries[5].id == entry_id { 6 });

    REQUIRE(scenario.entries[1].epoch_group == scenario.entries[2].epoch_group);
    REQUIRE(scenario.entries[3].epoch_group == scenario.entries[1].epoch_group);
    REQUIRE(scenario.entries[5].epoch_group != scenario.entries[3].epoch_group);
    REQUIRE(scenario.entries[3].kind == entry_kind::step_region);
    REQUIRE(scenario.entries[3].id == step);

    REQUIRE_THROWS_AS(
        builder.set_input("late", scalar(4.0), false), std::logic_error
    );
}

TEST_CASE("replay | A branch is immutable at every cut", "[replay][branch]")
{
    scenario_builder builder { run_id { 7 }, branch_id { 1 } };
    builder.mark_initialized(kernel::clock_tag::ui);
    builder.begin_epoch(kernel::clock_tag::ui);
    builder.set_input("a", scalar(1.0), false);
    builder.step_region(kernel::clock_tag::ui);
    builder.set_input("a", scalar(2.0), false);
    builder.step_region(kernel::clock_tag::ui);
    builder.step_region(kernel::clock_tag::audio);

    const auto base { builder.finish() };

    for (const auto& cut : base.entries)
    {
        branch_edit edit;
        edit.cut = cut.id;

        const auto branch { apply_branch(base, edit, branch_id { 2 }) };

        REQUIRE(branch.run == base.run);
        REQUIRE(branch.branch == branch_id { 2 });
        REQUIRE(branch.parent == base.run);
        REQUIRE(branch.base_entry == cut.id);

        REQUIRE(branch.entries.size() == base.entries.size());

        for (std::size_t i { 0 }; i < base.entries.size(); ++i)
        {
            REQUIRE(branch.entries[i].id == base.entries[i].id);
            REQUIRE(branch.entries[i].kind == base.entries[i].kind);
        }
    }

    REQUIRE(base.entries.size() == 6);
}

TEST_CASE(
    "replay | Branch edits insert, remove and replace suffixes",
    "[replay][branch]"
)
{
    scenario_builder builder { run_id { 3 }, branch_id { 1 } };
    builder.mark_initialized(kernel::clock_tag::ui);
    builder.begin_epoch(kernel::clock_tag::ui);
    builder.set_input("a", scalar(1.0), false);
    builder.step_region(kernel::clock_tag::ui);
    builder.set_input("a", scalar(2.0), false);
    builder.step_region(kernel::clock_tag::ui);
    builder.set_input("a", scalar(3.0), false);
    builder.step_region(kernel::clock_tag::ui);

    const auto base { builder.finish() };
    const auto original { base };

    scenario_entry inserted;
    inserted.kind = entry_kind::set_input;
    inserted.clock = kernel::clock_tag::ui;
    inserted.port = "a";
    inserted.value = scalar(10.0);

    scenario_entry replacement;
    replacement.kind = entry_kind::set_input;
    replacement.clock = kernel::clock_tag::ui;
    replacement.port = "a";
    replacement.value = scalar(20.0);

    branch_edit edit;
    edit.cut = base.entries[2].id;
    edit.inserted.push_back(inserted);
    edit.removed.push_back(base.entries[5].id);
    edit.replaced.emplace_back(base.entries[6].id, replacement);

    const auto branch { apply_branch(base, edit, branch_id { 9 }) };

    REQUIRE(base.entries.size() == original.entries.size());
    REQUIRE(branch.entries.size() == 7);

    REQUIRE(branch.entries[0].id == base.entries[0].id);
    REQUIRE(branch.entries[1].id == base.entries[1].id);
    REQUIRE(branch.entries[2].id == base.entries[2].id);

    REQUIRE(branch.entries[3].id != base.entries[3].id);
    REQUIRE(branch.entries[3].port == "a");
    REQUIRE(branch.entries[3].value.payload == scalar(10.0).payload);

    REQUIRE(branch.entries[4].id == base.entries[3].id);
    REQUIRE(branch.entries[5].id == base.entries[4].id);

    for (const auto& entry : branch.entries)
        REQUIRE(entry.id != base.entries[5].id);

    REQUIRE(branch.entries[6].id != base.entries[6].id);
    REQUIRE(branch.entries[6].value.payload == scalar(20.0).payload);
}

TEST_CASE("replay | Same-epoch input sets are unordered", "[replay][scenario]")
{
    scenario_builder first { run_id { 1 }, branch_id { 1 } };
    first.begin_epoch(kernel::clock_tag::ui);
    first.set_input("a", scalar(1.0), false);
    first.set_input("b", scalar(2.0), false);
    first.step_region(kernel::clock_tag::ui);

    scenario_builder second { run_id { 1 }, branch_id { 1 } };
    second.begin_epoch(kernel::clock_tag::ui);
    second.set_input("b", scalar(2.0), false);
    second.set_input("a", scalar(1.0), false);
    second.step_region(kernel::clock_tag::ui);

    const auto lhs { first.finish() };
    const auto rhs { second.finish() };

    std::map<std::pair<std::uint64_t, std::string>, encoded_value> lhs_sets;
    std::map<std::pair<std::uint64_t, std::string>, encoded_value> rhs_sets;

    for (const auto& entry : lhs.entries)
        if (entry.kind == entry_kind::set_input)
            lhs_sets.emplace(
                std::make_pair(entry.epoch_group, entry.port), entry.value
            );

    for (const auto& entry : rhs.entries)
        if (entry.kind == entry_kind::set_input)
            rhs_sets.emplace(
                std::make_pair(entry.epoch_group, entry.port), entry.value
            );

    REQUIRE(lhs_sets.size() == rhs_sets.size());

    for (const auto& [key, value] : lhs_sets)
    {
        const auto match { rhs_sets.find(key) };
        REQUIRE(match != rhs_sets.end());
        REQUIRE(encoded_value_equal(value, match->second));
    }
}

TEST_CASE(
    "replay | Incompleteness propagates through a branch",
    "[replay][branch]"
)
{
    scenario_builder builder { run_id { 5 }, branch_id { 1 } };
    builder.mark_initialized(kernel::clock_tag::ui);
    builder.mark_missing(2, "no codec");
    const auto base { builder.finish() };

    REQUIRE_FALSE(base.fidelity.complete);
    REQUIRE(base.fidelity.missing_entries == 2);
    REQUIRE(base.fidelity.reason == "no codec");

    branch_edit edit;
    edit.cut = base.entries.back().id;

    const auto branch { apply_branch(base, edit, branch_id { 2 }) };

    REQUIRE_FALSE(branch.fidelity.complete);
    REQUIRE(branch.fidelity.missing_entries == 2);
    REQUIRE(branch.fidelity.reason == "no codec");
}
