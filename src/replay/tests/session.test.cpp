#include "clockr/replay/session.hpp"
#include "clockr/replay/native.hpp"
#include "clockr/replay/replayer.hpp"

#include "test_support.hpp"

#include "clockr/kernel/release_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace clockr;
using namespace clockr::replay;

TEST_CASE(
    "replay | A session records a run that replays identically",
    "[replay][session]"
)
{
    codec_registry codecs;
    codecs.register_user<replay_test::point>("test.point");

    kernel::program program { replay_test::network() };
    kernel::release_engine engine;
    scenario_builder builder { run_id { 1 }, branch_id { 1 } };
    session session { program, engine, codecs, builder };

    session.mark_initialized();
    session.begin_epoch(kernel::clock_tag::ui);
    session.set("level", kernel::value { 2.0 });
    session.set<replay_test::point>("point", 1, 2);
    session.step(kernel::clock_tag::ui);
    session.step(kernel::clock_tag::audio);
    session.begin_epoch(kernel::clock_tag::ui);
    session.set("level", kernel::value { 3.0 });
    session.step(kernel::clock_tag::ui);
    session.step(kernel::clock_tag::audio);

    const auto recorded { builder.finish() };
    REQUIRE(recorded.fidelity.complete);
    REQUIRE(recorded.entries.size() == 8);

    using recipe_t = decltype(replay_test::network());
    replay::native::recipe_factory<recipe_t> factory { replay_test::network() };
    replayer replayer;

    const auto run { replayer.replay(recorded, factory, codecs) };

    REQUIRE_FALSE(run.aborted);
    REQUIRE(run.fidelity.complete);
    REQUIRE(run.steps.size() == 4);
    REQUIRE(std::get<double>(run.steps[1].outputs[0].payload) == 2.0);
    REQUIRE(std::get<double>(run.steps[3].outputs[0].payload) == 5.0);
    REQUIRE(
        std::holds_alternative<std::monostate>(run.steps[0].outputs[0].payload)
    );
    REQUIRE(
        std::holds_alternative<user_bytes>(run.steps[0].outputs[1].payload)
    );
}

TEST_CASE("replay | A late-start session is incomplete", "[replay][session]")
{
    codec_registry codecs;
    kernel::program program { replay_test::network() };
    kernel::release_engine engine;

    program.set("level", kernel::value { 0.0 });
    program.set<replay_test::point>("point", 0, 0);
    engine.step(program, kernel::clock_tag::ui);

    scenario_builder builder { run_id { 2 }, branch_id { 1 } };
    session session { program, engine, codecs, builder };

    session.mark_initialized();

    const auto recorded { builder.finish() };
    REQUIRE_FALSE(recorded.fidelity.complete);
    REQUIRE(recorded.fidelity.reason.find("late start") != std::string::npos);
}

TEST_CASE(
    "replay | A missing codec marks the journal incomplete",
    "[replay][session]"
)
{
    codec_registry codecs;
    kernel::program program { replay_test::network() };
    kernel::release_engine engine;
    scenario_builder builder { run_id { 3 }, branch_id { 1 } };
    session session { program, engine, codecs, builder };

    session.mark_initialized();
    session.begin_epoch(kernel::clock_tag::ui);
    session.set("level", kernel::value { 0.0 });
    session.set<replay_test::point>("point", 4, 5);
    session.step(kernel::clock_tag::ui);
    session.step(kernel::clock_tag::audio);

    const auto recorded { builder.finish() };

    REQUIRE_FALSE(recorded.fidelity.complete);
    REQUIRE(recorded.fidelity.missing_entries == 1);
    REQUIRE(recorded.fidelity.reason.find("test.point") == std::string::npos);
    REQUIRE(
        recorded.fidelity.reason.find("no replay codec") != std::string::npos
    );

    for (const auto& entry : recorded.entries)
        if (entry.kind == entry_kind::set_input)
            REQUIRE(entry.port != "point");
}

TEST_CASE(
    "replay | A throwing tick is recorded and rethrown",
    "[replay][session]"
)
{
    codec_registry codecs;
    kernel::program program { replay_test::throwing_network() };
    kernel::release_engine engine;
    scenario_builder builder { run_id { 4 }, branch_id { 1 } };
    session session { program, engine, codecs, builder };

    session.mark_initialized();
    session.begin_epoch(kernel::clock_tag::ui);
    session.set("level", kernel::value { 1.0 });

    REQUIRE_THROWS_AS(session.step(kernel::clock_tag::ui), std::runtime_error);
    REQUIRE(session.aborted());

    const auto recorded { builder.finish() };
    REQUIRE_FALSE(recorded.fidelity.complete);

    const auto* step { static_cast<const scenario_entry*>(nullptr) };

    for (const auto& entry : recorded.entries)
        if (entry.kind == entry_kind::step_region)
            step = &entry;

    REQUIRE(step != nullptr);
    REQUIRE(step->threw);
}
