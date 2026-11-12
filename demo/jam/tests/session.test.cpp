// jam session — authoritative revisions, stale rejection, review marks, and
// deterministic fresh-program renders.

#include "jam/session.hpp"
#include "jam/network.hpp"

#include "clockr/replay/compare.hpp"
#include "clockr/replay/native.hpp"

#include "fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

using clockr::replay::branch_id;
using clockr::replay::encoded_value_equal;
using clockr::replay::scenario_entry;

jam::score_patch
set_note(std::uint8_t note)
{
    return jam_test::make_patch(
        {
            jam::patch_step { jam::patch_op::set_step, 0, 0, note, 100 },
        }
    );
}

jam::score_patch
transpose(std::int16_t semitones)
{
    return jam_test::make_patch(
        {
            jam::patch_step { jam::patch_op::transpose, 0, 0, 0, semitones },
        }
    );
}

bool
same_entries(
    const std::vector<scenario_entry>& lhs,
    const std::vector<scenario_entry>& rhs
)
{
    if (lhs.size() != rhs.size())
        return false;

    for (std::size_t i { 0 }; i < lhs.size(); ++i)
    {
        if (lhs[i].id != rhs[i].id || lhs[i].kind != rhs[i].kind ||
            lhs[i].clock != rhs[i].clock || lhs[i].port != rhs[i].port ||
            lhs[i].note != rhs[i].note || lhs[i].is_event != rhs[i].is_event ||
            lhs[i].threw != rhs[i].threw)
            return false;

        if (!encoded_value_equal(lhs[i].value, rhs[i].value))
            return false;
    }

    return true;
}

std::size_t
patch_entries(const clockr::replay::scenario& scenario)
{
    std::size_t count { 0 };

    for (const auto& entry : scenario.entries)
        if (entry.kind == clockr::replay::entry_kind::set_input &&
            entry.port == "patch")
            ++count;

    return count;
}

} // namespace

TEST_CASE(
    "jam/session | Accepted proposal advances head and enters history",
    "[jam][session]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };
    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::codec_registry codecs;
    jam::session session { initial, factory, codecs };

    std::string error;
    REQUIRE(session.head().value == 0);
    REQUIRE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, set_note(72), error
    ));
    REQUIRE(error.empty());

    REQUIRE(session.head().value == 1);
    REQUIRE(session.actions().size() == 1);
    REQUIRE(session.actions()[0].author.id == "alice");
    REQUIRE(session.actions()[0].base.value == 0);
    REQUIRE(session.current_score().patterns[0].steps[0].note == 72);

    const auto& history { session.history() };
    REQUIRE(history.fidelity.complete);
    REQUIRE(patch_entries(history) == 1);
}

TEST_CASE(
    "jam/session | Stale base revision is rejected without a change",
    "[jam][session]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };
    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::codec_registry codecs;
    jam::session session { initial, factory, codecs };

    std::string error;
    REQUIRE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, set_note(72), error
    ));

    const auto head_before { session.head() };
    const auto entries_before { session.history().entries };

    REQUIRE_FALSE(session.propose(
        jam::participant { "bob" }, jam::revision { 0 }, set_note(60), error
    ));

    REQUIRE(error.find("stale") != std::string::npos);
    REQUIRE(session.head() == head_before);
    REQUIRE(session.actions().size() == 1);
    REQUIRE(same_entries(session.history().entries, entries_before));
}

TEST_CASE(
    "jam/session | Invalid patch capacity and indices are rejected",
    "[jam][session]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };
    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::codec_registry codecs;
    jam::session session { initial, factory, codecs };

    std::string error;

    jam::score_patch too_many {};
    too_many.count = static_cast<std::uint8_t>(jam::patch_capacity + 1);
    REQUIRE_FALSE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, too_many, error
    ));
    REQUIRE(error.find("capacity") != std::string::npos);
    REQUIRE(session.head().value == 0);

    jam::score_patch bad_pattern {};
    bad_pattern.count = 1;
    bad_pattern.ops[0] = jam::patch_step {
        jam::patch_op::set_step,
        static_cast<std::uint8_t>(jam::pattern_count),
        0,
        60,
        100,
    };
    REQUIRE_FALSE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, bad_pattern, error
    ));
    REQUIRE(error.find("out of range") != std::string::npos);
    REQUIRE(session.head().value == 0);

    jam::score_patch bad_step {};
    bad_step.count = 1;
    bad_step.ops[0] = jam::patch_step {
        jam::patch_op::set_step,
        0,
        static_cast<std::uint8_t>(jam::steps_per_pattern),
        60,
        100,
    };
    REQUIRE_FALSE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, bad_step, error
    ));
    REQUIRE(error.find("out of range") != std::string::npos);
    REQUIRE(session.head().value == 0);
    REQUIRE(session.history().entries.size() > 0);
    REQUIRE(patch_entries(session.history()) == 0);
}

TEST_CASE(
    "jam/session | A conflicting proposal against the old base is stale",
    "[jam][session]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };
    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::codec_registry codecs;
    jam::session session { initial, factory, codecs };

    std::string error;
    REQUIRE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, set_note(72), error
    ));
    REQUIRE(session.head().value == 1);

    REQUIRE_FALSE(session.propose(
        jam::participant { "bob" }, jam::revision { 0 }, transpose(5), error
    ));

    REQUIRE(error.find("stale") != std::string::npos);
    REQUIRE(session.head().value == 1);
    REQUIRE(session.actions().size() == 1);
}

TEST_CASE("jam/session | Head render is deterministic", "[jam][session]")
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };
    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::codec_registry codecs;
    jam::session session { initial, factory, codecs };

    std::string error;
    REQUIRE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, set_note(72), error
    ));
    REQUIRE(session.propose(
        jam::participant { "bob" },
        jam::revision { session.head() },
        transpose(7),
        error
    ));

    const auto head { session.head() };
    const auto first { session.render(branch_id { head.value }) };
    const auto second { session.render(branch_id { head.value }) };

    REQUIRE(first.fidelity.complete);
    REQUIRE(clockr::replay::compare(first, second).equal);
}

TEST_CASE(
    "jam/session | A patched revision renders differently and history stays "
    "put",
    "[jam][session]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };
    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::codec_registry codecs;
    jam::session session { initial, factory, codecs };

    std::string error;
    REQUIRE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, set_note(72), error
    ));

    const auto entries_before { session.history().entries };

    const auto raw { session.render(branch_id { 0 }) };
    const auto edited { session.render(branch_id { 1 }) };

    REQUIRE(raw.fidelity.complete);
    REQUIRE(edited.fidelity.complete);
    REQUIRE_FALSE(clockr::replay::compare(raw, edited).equal);

    REQUIRE(same_entries(session.history().entries, entries_before));
    REQUIRE(session.head().value == 1);

    // Beyond the head clamps to the head.
    const auto clamped { session.render(branch_id { 1000 }) };
    REQUIRE(clockr::replay::compare(clamped, edited).equal);
}

TEST_CASE(
    "jam/session | An accept decision appears in history",
    "[jam][session]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };
    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::codec_registry codecs;
    jam::session session { initial, factory, codecs };

    std::string error;
    REQUIRE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, set_note(72), error
    ));
    REQUIRE(patch_entries(session.history()) == 1);

    session.accept(jam::revision { 1 });

    bool found { false };

    for (const auto& entry : session.history().entries)
        if (entry.kind == clockr::replay::entry_kind::mark &&
            entry.note == "accept revision 1")
            found = true;

    REQUIRE(found);
    REQUIRE(session.reviews().size() == 1);
    REQUIRE(session.reviews()[0].target.value == 1);

    // The audit marks append; the audible patch log is unchanged.
    REQUIRE(patch_entries(session.history()) == 1);
    REQUIRE(session.current_score().patterns[0].steps[0].note == 72);
}

TEST_CASE(
    "jam/session | Head render is complete and has the expected steps",
    "[jam][session]"
)
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };
    clockr::replay::native::recipe_factory factory { recipe };
    clockr::replay::codec_registry codecs;
    jam::session session { initial, factory, codecs };

    std::string error;
    REQUIRE(session.propose(
        jam::participant { "alice" }, jam::revision { 0 }, set_note(72), error
    ));
    REQUIRE(session.propose(
        jam::participant { "bob" },
        jam::revision { session.head() },
        transpose(7),
        error
    ));

    const auto head { session.head() };
    const auto run { session.render(branch_id { head.value }) };

    REQUIRE(run.fidelity.complete);
    REQUIRE_FALSE(run.aborted);

    // One initial ui step, ui+audio per revision, and one trailing audio tick.
    REQUIRE(run.steps.size() == 2 + 2 * head.value);
}
