#include "clockr/replay/compare.hpp"
#include "clockr/replay/native.hpp"
#include "clockr/replay/replayer.hpp"
#include "clockr/replay/scenario.hpp"
#include "clockr/replay/session.hpp"
#include "clockr/replay/value_codec.hpp"

#include "daw/dawmulator.hpp"

#include "clockr/kernel/clock.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

using clockr::kernel::clock_tag;
using clockr::replay::encoded_value;
using clockr::replay::entry_id;
using clockr::replay::run;
using clockr::replay::step_record;

struct recorder
{
    run reference;
    std::array<std::uint64_t, clockr::kernel::clock_tag_count> ordinals {};

    void
    capture(
        clockr::kernel::program& program,
        const clockr::replay::codec_registry& codecs,
        clock_tag clock
    )
    {
        const auto index { clockr::kernel::clock_tag_index(clock) };

        step_record record;
        record.step = clockr::replay::region_step_id { clock,
                                                       ordinals[index]++ };
        record.epoch = program.region(clock).published_epoch.load();

        const auto output_count { program.descriptor().output_ids.size() };

        for (std::size_t i { 0 }; i < output_count; ++i)
        {
            encoded_value encoded;
            std::string error;

            if (!clockr::replay::encode_value(
                    program.get(i), codecs, encoded, error
                ))
                throw std::runtime_error { error };

            record.outputs.push_back(std::move(encoded));
        }

        reference.steps.push_back(std::move(record));
    }
};

void
record_daw_session(
    clockr::kernel::program& program,
    clockr::kernel::engine& engine,
    clockr::replay::codec_registry& codecs,
    clockr::replay::scenario_builder& builder,
    recorder& recorded
)
{
    clockr::replay::session session { program, engine, codecs, builder };

    session.begin_epoch(clock_tag::ui);
    session.mark_initialized();
    session.set("playing", clockr::kernel::value { false });
    session.set("bpm", clockr::kernel::value { 120.0 });
    session.set<daw::LoopRegion>("loop", 0.0, 4.0, true);
    session.set("master", clockr::kernel::value { 0.8 });
    session.step(clock_tag::ui);
    recorded.capture(program, codecs, clock_tag::ui);

    for (std::int64_t i { 0 }; i < 8; ++i)
    {
        if (i == 2 || i == 5)
        {
            session.begin_epoch(clock_tag::ui);

            if (i == 2)
                session.set("playing", clockr::kernel::value { true });

            if (i == 5)
                session.set("seek", clockr::kernel::value { 3.0 });

            session.step(clock_tag::ui);
            recorded.capture(program, codecs, clock_tag::ui);
        }

        session.begin_epoch(clock_tag::audio);
        session.set<daw::BlockDesc>(
            "block", i * 256, std::int64_t { 256 }, 48000.0
        );
        session.step(clock_tag::audio);
        recorded.capture(program, codecs, clock_tag::audio);
    }

    recorded.reference.run = builder.peek().run;
    recorded.reference.branch = builder.peek().branch;
    recorded.reference.fidelity = builder.peek().fidelity;
}

const clockr::replay::scenario_entry*
find_entry(
    const clockr::replay::scenario& scenario,
    std::string_view port,
    std::size_t occurrence
)
{
    std::size_t seen { 0 };

    for (const auto& entry : scenario.entries)
    {
        if (entry.kind != clockr::replay::entry_kind::set_input ||
            entry.port != port)
            continue;

        if (seen++ == occurrence)
            return &entry;
    }

    return nullptr;
}

} // namespace

TEST_CASE(
    "daw/replay | Recorded DAW scenario replays identically and twice",
    "[daw][replay]"
)
{
    const auto recipe { daw::dawmulator_recipe() };

    clockr::kernel::program_options options;
    options.region_pool_bytes = 1u << 20;

    clockr::kernel::program program { recipe, options };
    clockr::kernel::release_engine engine;

    clockr::replay::codec_registry codecs;
    codecs.register_user<daw::LoopRegion>("daw.loop_region");
    codecs.register_user<daw::BlockDesc>("daw.block_desc");

    clockr::replay::scenario_builder builder {
        clockr::replay::run_id { 41 },
        clockr::replay::branch_id { 1 },
    };

    recorder recorded;
    record_daw_session(program, engine, codecs, builder, recorded);

    const auto scenario { builder.finish() };
    REQUIRE(scenario.fidelity.complete);
    REQUIRE(!scenario.entries.empty());

    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::replayer replayer;
    clockr::replay::replay_options replay_options;
    replay_options.capture_trace = false;

    const auto first {
        replayer.replay(scenario, factory, codecs, replay_options)
    };
    REQUIRE(first.fidelity.complete);
    REQUIRE(!first.aborted);
    REQUIRE(first.steps.size() == recorded.reference.steps.size());

    const auto diff { clockr::replay::compare(
        recorded.reference, first
    ) };
    INFO(diff.summary);
    REQUIRE(diff.equal);

    const auto second {
        replayer.replay(scenario, factory, codecs, replay_options)
    };

    REQUIRE(clockr::replay::compare(first, second).equal);
}

TEST_CASE(
    "daw/replay | Branch replaces a suffix boundary event without mutating the base",
    "[daw][replay]"
)
{
    const auto recipe { daw::dawmulator_recipe() };

    clockr::kernel::program_options options;
    options.region_pool_bytes = 1u << 20;

    clockr::kernel::program program { recipe, options };
    clockr::kernel::release_engine engine;

    clockr::replay::codec_registry codecs;
    codecs.register_user<daw::LoopRegion>("daw.loop_region");
    codecs.register_user<daw::BlockDesc>("daw.block_desc");

    clockr::replay::scenario_builder builder {
        clockr::replay::run_id { 51 },
        clockr::replay::branch_id { 1 },
    };

    recorder recorded;
    record_daw_session(program, engine, codecs, builder, recorded);

    const auto base { builder.finish() };

    const auto* target { find_entry(base, "block", 4) };
    REQUIRE(target != nullptr);

    const auto saved_value { target->value };

    clockr::replay::branch_edit edit;

    for (const auto& entry : base.entries)
    {
        if (entry.id < target->id)
            edit.cut = entry.id;
    }

    auto replacement { *target };
    clockr::replay::encoded_value changed;
    std::string error;
    const auto changed_value {
        clockr::kernel::make_user_value<daw::BlockDesc>(
            nullptr, 4 * 128, std::int64_t { 128 }, 48000.0
        )
    };

    REQUIRE(clockr::replay::encode_value(
        changed_value, codecs, changed, error
    ));
    clockr::kernel::release(changed_value);

    replacement.value = std::move(changed);
    edit.replaced.emplace_back(target->id, std::move(replacement));

    const auto branched { clockr::replay::apply_branch(
        base, edit, clockr::replay::branch_id { 2 }
    ) };

    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::replayer replayer;
    clockr::replay::replay_options replay_options;
    replay_options.capture_trace = false;

    const auto original_run {
        replayer.replay(base, factory, codecs, replay_options)
    };
    const auto branch_run {
        replayer.replay(branched, factory, codecs, replay_options)
    };

    REQUIRE(branch_run.branch == clockr::replay::branch_id { 2 });
    const auto branch_diff {
        clockr::replay::compare(original_run, branch_run)
    };
    INFO(branch_diff.summary);
    INFO(
        "first_step=" << branch_diff.first_step
                      << " outputs=" << branch_diff.output_mismatches
                      << " target=" << target->id.value
                      << " branched_entries=" << branched.entries.size()
    );
    REQUIRE(!branch_diff.equal);

    REQUIRE(base.entries.size() == branched.entries.size());

    bool found { false };

    for (const auto& entry : base.entries)
        if (entry.id == target->id)
        {
            found = true;
            REQUIRE(clockr::replay::encoded_value_equal(
                entry.value, saved_value
            ));
        }

    REQUIRE(found);
}

TEST_CASE(
    "daw/replay | Pure cut branch reproduces the unchanged run",
    "[daw][replay]"
)
{
    const auto recipe { daw::dawmulator_recipe() };

    clockr::kernel::program_options options;
    options.region_pool_bytes = 1u << 20;

    clockr::kernel::program program { recipe, options };
    clockr::kernel::release_engine engine;

    clockr::replay::codec_registry codecs;
    codecs.register_user<daw::LoopRegion>("daw.loop_region");
    codecs.register_user<daw::BlockDesc>("daw.block_desc");

    clockr::replay::scenario_builder builder {
        clockr::replay::run_id { 61 },
        clockr::replay::branch_id { 1 },
    };

    recorder recorded;
    record_daw_session(program, engine, codecs, builder, recorded);

    const auto base { builder.finish() };

    clockr::replay::branch_edit edit;
    edit.cut = base.entries.back().id;

    const auto branched { clockr::replay::apply_branch(
        base, edit, clockr::replay::branch_id { 2 }
    ) };

    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::replayer replayer;
    clockr::replay::replay_options replay_options;
    replay_options.capture_trace = false;

    const auto original_run {
        replayer.replay(base, factory, codecs, replay_options)
    };
    const auto branch_run {
        replayer.replay(branched, factory, codecs, replay_options)
    };

    REQUIRE(clockr::replay::compare(original_run, branch_run).equal);
}
