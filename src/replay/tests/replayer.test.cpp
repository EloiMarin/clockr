#include "clockr/replay/replayer.hpp"
#include "clockr/replay/compare.hpp"
#include "clockr/replay/native.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <variant>
#include <vector>

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

scenario
recorded_pair_scenario()
{
    scenario_builder builder { run_id { 10 }, branch_id { 1 } };
    builder.mark_initialized(kernel::clock_tag::ui);
    builder.begin_epoch(kernel::clock_tag::ui);
    builder.set_input("level", scalar(2.0), false);
    builder.step_region(kernel::clock_tag::ui);
    builder.step_region(kernel::clock_tag::audio);
    builder.begin_epoch(kernel::clock_tag::ui);
    builder.set_input("level", scalar(3.0), false);
    builder.step_region(kernel::clock_tag::ui);
    builder.step_region(kernel::clock_tag::audio);
    return builder.finish();
}

} // namespace

TEST_CASE(
    "replay | Unchanged replay reproduces outputs and trace",
    "[replay][replayer]"
)
{
    codec_registry codecs;
    codecs.register_user<replay_test::point>("test.point");

    const auto scenario { recorded_pair_scenario() };
    using recipe_t = decltype(replay_test::network());

    replay::native::recipe_factory<recipe_t> factory { replay_test::network() };
    replayer replayer;

    replay_options options;
    options.capture_trace = true;

    const auto first { replayer.replay(scenario, factory, codecs, options) };
    const auto second { replayer.replay(scenario, factory, codecs, options) };

    REQUIRE_FALSE(first.aborted);
    REQUIRE(first.fidelity.complete);
    REQUIRE(first.steps.size() == 4);
    REQUIRE(std::get<double>(first.steps[1].outputs[0].payload) == 2.0);
    REQUIRE(std::get<double>(first.steps[3].outputs[0].payload) == 5.0);
    REQUIRE_FALSE(first.trace.empty());

    const auto diff { compare(first, second) };
    REQUIRE(diff.equal);
    REQUIRE(diff.trace_mismatches == 0);
    REQUIRE(diff.output_mismatches == 0);
}

TEST_CASE("replay | A branch changes only the suffix", "[replay][replayer]")
{
    codec_registry codecs;
    codecs.register_user<replay_test::point>("test.point");

    const auto base { recorded_pair_scenario() };
    const auto original { base };

    branch_edit edit;
    edit.cut = base.entries[3].id;

    for (const auto& entry : base.entries)
        if (entry.id > edit.cut)
            edit.removed.push_back(entry.id);

    scenario_entry set;
    set.kind = entry_kind::set_input;
    set.clock = kernel::clock_tag::ui;
    set.port = "level";
    set.value = scalar(10.0);

    scenario_entry ui_step;
    ui_step.kind = entry_kind::step_region;
    ui_step.clock = kernel::clock_tag::ui;

    scenario_entry audio_step;
    audio_step.kind = entry_kind::step_region;
    audio_step.clock = kernel::clock_tag::audio;

    edit.inserted.push_back(set);
    edit.inserted.push_back(ui_step);
    edit.inserted.push_back(audio_step);

    using recipe_t = decltype(replay_test::network());

    replay::native::recipe_factory<recipe_t> factory { replay_test::network() };
    replayer replayer;

    const auto base_run { replayer.replay(base, factory, codecs) };
    const auto branch_run {
        replayer.replay_branch(base, edit, branch_id { 2 }, factory, codecs)
    };

    REQUIRE(base.entries.size() == original.entries.size());
    REQUIRE(base.entries[4].value.payload == original.entries[4].value.payload);

    REQUIRE(std::get<double>(base_run.steps[3].outputs[0].payload) == 5.0);
    REQUIRE(std::get<double>(branch_run.steps[3].outputs[0].payload) == 12.0);

    const auto diff { compare(base_run, branch_run) };
    REQUIRE_FALSE(diff.equal);
    REQUIRE(diff.first_step == 3);
    REQUIRE(diff.output_mismatches > 0);
}

TEST_CASE(
    "replay | A throwing replay step is represented",
    "[replay][replayer]"
)
{
    codec_registry codecs;

    scenario_builder builder { run_id { 11 }, branch_id { 1 } };
    builder.mark_initialized(kernel::clock_tag::ui);
    builder.begin_epoch(kernel::clock_tag::ui);
    builder.set_input("level", scalar(1.0), false);
    builder.step_region(kernel::clock_tag::ui);
    builder.step_region(kernel::clock_tag::ui);
    const auto scenario { builder.finish() };

    using recipe_t = decltype(replay_test::throwing_network());

    replay::native::recipe_factory<recipe_t> factory {
        replay_test::throwing_network()
    };
    replayer replayer;

    const auto stopped { replayer.replay(scenario, factory, codecs) };
    REQUIRE(stopped.aborted);
    REQUIRE(stopped.steps.size() == 1);
    REQUIRE(stopped.steps[0].threw);
    REQUIRE_FALSE(stopped.fidelity.complete);

    replay_options options;
    options.stop_on_throw = false;

    const auto continued {
        replayer.replay(scenario, factory, codecs, options)
    };
    REQUIRE(continued.aborted);
    REQUIRE(continued.steps.size() == 2);
    REQUIRE(continued.steps[0].threw);
    REQUIRE(continued.steps[1].threw);
}

TEST_CASE("replay | max_steps truncates a run", "[replay][replayer]")
{
    codec_registry codecs;
    const auto scenario { recorded_pair_scenario() };

    using recipe_t = decltype(replay_test::network());

    replay::native::recipe_factory<recipe_t> factory { replay_test::network() };
    replayer replayer;

    replay_options options;
    options.max_steps = 1;

    const auto run { replayer.replay(scenario, factory, codecs, options) };

    REQUIRE(run.steps.size() == 1);
    REQUIRE_FALSE(run.fidelity.complete);
    REQUIRE(run.fidelity.reason.find("max_steps") != std::string::npos);
}
