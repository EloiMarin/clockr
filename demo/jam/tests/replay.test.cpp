// jam replay integration — the replay substrate on the jam network, kept
// independent of jam::session so a failure localizes to the substrate or the
// network rather than to either.

#include "jam/network.hpp"
#include "jam/patch.hpp"
#include "jam/score.hpp"

#include "clockr/replay/compare.hpp"
#include "clockr/replay/native.hpp"
#include "clockr/replay/replayer.hpp"
#include "clockr/replay/scenario.hpp"
#include "clockr/replay/session.hpp"
#include "clockr/replay/value_codec.hpp"

#include "clockr/kernel/clock.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"

#include "fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using clockr::kernel::clock_tag;
using clockr::replay::encoded_value;
using clockr::replay::run;
using clockr::replay::scenario_entry;
using clockr::replay::step_record;

constexpr std::int64_t kRate { 48000 };
constexpr std::int64_t kBlock { 6000 }; // one 16th at 120 bpm

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
        record.step =
            clockr::replay::region_step_id { clock, ordinals[index]++ };
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
record_jam_session(
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
    session.set("playing", clockr::kernel::value { true });
    session.set("bpm", clockr::kernel::value { 120.0 });
    session.set<daw::LoopRegion>("loop", 0.0, 1024.0, true);
    session.set("master", clockr::kernel::value { 0.8 });
    session.step(clock_tag::ui);
    recorded.capture(program, codecs, clock_tag::ui);

    const auto patch { jam_test::make_patch(
        {
            jam::patch_step { jam::patch_op::transpose, 0, 0, 0, 12 },
        }
    ) };

    for (std::int64_t i { 0 }; i < 8; ++i)
    {
        if (i == 2)
        {
            session.begin_epoch(clock_tag::ui);
            session.set<jam::score_patch>("patch", patch);
            session.step(clock_tag::ui);
            recorded.capture(program, codecs, clock_tag::ui);
        }

        session.begin_epoch(clock_tag::audio);
        session.set<daw::BlockDesc>(
            "block", i * kBlock, kBlock, static_cast<double>(kRate)
        );
        session.step(clock_tag::audio);
        recorded.capture(program, codecs, clock_tag::audio);
    }

    const auto& peek { builder.peek() };
    recorded.reference.run = peek.run;
    recorded.reference.branch = peek.branch;
    recorded.reference.fidelity = peek.fidelity;
}

const scenario_entry*
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

encoded_value
encode_patch(
    const jam::score_patch& patch,
    const clockr::replay::codec_registry& codecs
)
{
    const auto value {
        clockr::kernel::make_user_value<jam::score_patch>(nullptr, patch)
    };

    encoded_value encoded;
    std::string error;
    const bool ok {
        clockr::replay::encode_value(value, codecs, encoded, error)
    };

    clockr::kernel::release(value);

    if (!ok)
        throw std::runtime_error { error };

    return encoded;
}

} // namespace

TEST_CASE(
    "jam/replay | A scripted jam performance replays identically twice",
    "[jam][replay]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };

    clockr::kernel::program_options options;
    options.region_pool_bytes = std::size_t { 1 } << 20;

    clockr::kernel::program program { recipe, options };
    clockr::kernel::release_engine engine;

    clockr::replay::codec_registry codecs;
    codecs.register_user<jam::score_patch>("jam.score_patch");
    codecs.register_user<jam::event_batch>("jam.event_batch");
    codecs.register_user<jam::voice_state>("jam.voice_state");
    codecs.register_user<daw::LoopRegion>("daw.loop_region");
    codecs.register_user<daw::BlockDesc>("daw.block_desc");

    clockr::replay::scenario_builder builder {
        clockr::replay::run_id { 71 },
        clockr::replay::branch_id { 1 },
    };

    recorder recorded;
    record_jam_session(program, engine, codecs, builder, recorded);

    const auto scenario { builder.finish() };
    REQUIRE(scenario.fidelity.complete);
    REQUIRE(!scenario.entries.empty());

    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::replayer replayer;
    clockr::replay::replay_options replay_options;
    replay_options.capture_trace = false;
    replay_options.program_options.region_pool_bytes = std::size_t { 1 } << 20;

    const auto first {
        replayer.replay(scenario, factory, codecs, replay_options)
    };
    REQUIRE(first.fidelity.complete);
    REQUIRE(!first.aborted);
    REQUIRE(first.steps.size() == recorded.reference.steps.size());

    const auto live_diff { clockr::replay::compare(recorded.reference, first) };
    INFO(live_diff.summary);
    REQUIRE(live_diff.equal);

    const auto second {
        replayer.replay(scenario, factory, codecs, replay_options)
    };

    REQUIRE(clockr::replay::compare(first, second).equal);
}

TEST_CASE(
    "jam/replay | A branch patch changes the suffix, not the base",
    "[jam][replay]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };

    clockr::kernel::program_options options;
    options.region_pool_bytes = std::size_t { 1 } << 20;

    clockr::kernel::program program { recipe, options };
    clockr::kernel::release_engine engine;

    clockr::replay::codec_registry codecs;
    codecs.register_user<jam::score_patch>("jam.score_patch");
    codecs.register_user<jam::event_batch>("jam.event_batch");
    codecs.register_user<jam::voice_state>("jam.voice_state");
    codecs.register_user<daw::LoopRegion>("daw.loop_region");
    codecs.register_user<daw::BlockDesc>("daw.block_desc");

    clockr::replay::scenario_builder builder {
        clockr::replay::run_id { 72 },
        clockr::replay::branch_id { 1 },
    };

    recorder recorded;
    record_jam_session(program, engine, codecs, builder, recorded);

    const auto base { builder.finish() };
    const auto* target { find_entry(base, "patch", 0) };
    REQUIRE(target != nullptr);

    const auto saved_value { target->value };

    clockr::replay::branch_edit edit;

    for (const auto& entry : base.entries)
        if (entry.id < target->id)
            edit.cut = entry.id;

    auto replacement { *target };
    replacement.value = encode_patch(
        jam_test::make_patch(
            {
                jam::patch_step { jam::patch_op::transpose, 0, 0, 0, 7 },
            }
        ),
        codecs
    );
    edit.replaced.emplace_back(target->id, std::move(replacement));

    const auto branched { clockr::replay::apply_branch(
        base, edit, clockr::replay::branch_id { 2 }
    ) };

    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::replayer replayer;
    clockr::replay::replay_options replay_options;
    replay_options.capture_trace = false;
    replay_options.program_options.region_pool_bytes = std::size_t { 1 } << 20;

    const auto original_run {
        replayer.replay(base, factory, codecs, replay_options)
    };
    const auto branch_run {
        replayer.replay(branched, factory, codecs, replay_options)
    };

    REQUIRE(branch_run.branch == clockr::replay::branch_id { 2 });

    const auto diff { clockr::replay::compare(original_run, branch_run) };
    INFO(diff.summary);
    INFO("first_step=" << diff.first_step);
    REQUIRE_FALSE(diff.equal);
    REQUIRE(diff.first_step > 0);

    REQUIRE(base.entries.size() == branched.entries.size());

    bool found { false };

    for (const auto& entry : base.entries)
        if (entry.id == target->id)
        {
            found = true;
            REQUIRE(
                clockr::replay::encoded_value_equal(entry.value, saved_value)
            );
        }

    REQUIRE(found);
}
