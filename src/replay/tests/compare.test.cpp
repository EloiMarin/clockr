#include "clockr/replay/compare.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace clockr;
using namespace clockr::replay;
using clockr::kernel::clock_tag;
using clockr::kernel::node_id;

namespace
{

encoded_value
scalar(double value)
{
    encoded_value encoded;
    encoded.payload = value;
    return encoded;
}

encoded_value
user(std::uint32_t type_id, std::vector<std::byte> bytes)
{
    user_bytes payload;
    payload.type_id = type_id;
    payload.bytes = std::move(bytes);

    encoded_value encoded;
    encoded.payload = std::move(payload);
    return encoded;
}

step_record
step(clock_tag clock, std::uint64_t ordinal, encoded_value output)
{
    step_record record;
    record.step = region_step_id { clock, ordinal };
    record.epoch = ordinal + 1;
    record.outputs.push_back(std::move(output));
    return record;
}

kernel::trace_event
event(node_id id, double value, std::uint64_t sequence, std::uint64_t duration)
{
    return kernel::trace_event { kernel::trace_event_kind::node_tick_end,
                                 clock_tag::ui,
                                 1,
                                 id,
                                 sequence,
                                 kernel::value { value },
                                 duration };
}

} // namespace

TEST_CASE("replay | Identical runs compare equal", "[replay][compare]")
{
    run expected;
    expected.steps.push_back(step(clock_tag::ui, 0, scalar(1.0)));
    expected.steps.push_back(step(clock_tag::audio, 0, scalar(2.0)));
    expected.trace.push_back(event(3, 1.0, 1, 20));

    run actual { expected };
    actual.steps[0].epoch = 99;
    actual.trace[0].sequence = 100;
    actual.trace[0].duration_ns = 5000;

    const auto diff { compare(expected, actual) };

    REQUIRE(diff.equal);
    REQUIRE(diff.output_mismatches == 0);
    REQUIRE(diff.trace_mismatches == 0);
    REQUIRE(diff.summary == "runs are semantically equal");
}

TEST_CASE(
    "replay | A changed output reports its first step",
    "[replay][compare]"
)
{
    run expected;
    expected.steps.push_back(step(clock_tag::ui, 0, scalar(1.0)));
    expected.steps.push_back(step(clock_tag::ui, 1, scalar(2.0)));
    expected.steps.push_back(step(clock_tag::ui, 2, scalar(3.0)));

    run actual { expected };
    actual.steps[1].outputs[0] = scalar(20.0);

    const auto diff { compare(expected, actual) };

    REQUIRE_FALSE(diff.equal);
    REQUIRE(diff.first_step == 1);
    REQUIRE(diff.output_mismatches == 1);
    REQUIRE(diff.values.size() == 1);
    REQUIRE(diff.values[0].step == 1);
    REQUIRE(diff.values[0].port == 0);
    REQUIRE(std::get<double>(diff.values[0].expected.payload) == 2.0);
    REQUIRE(std::get<double>(diff.values[0].actual.payload) == 20.0);
}

TEST_CASE(
    "replay | Equal user bytes compare equal regardless of type id order",
    "[replay][compare]"
)
{
    run expected;
    expected.steps.push_back(step(
        clock_tag::ui,
        0,
        user(7, { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } })
    ));

    run actual { expected };
    actual.steps[0].outputs[0] =
        user(7, { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } });

    REQUIRE(compare(expected, actual).equal);

    actual.steps[0].outputs[0] =
        user(7, { std::byte { 1 }, std::byte { 2 }, std::byte { 4 } });

    const auto diff { compare(expected, actual) };
    REQUIRE_FALSE(diff.equal);
    REQUIRE(diff.output_mismatches == 1);
}

TEST_CASE("replay | Size differences are not hidden", "[replay][compare]")
{
    run expected;
    expected.steps.push_back(step(clock_tag::ui, 0, scalar(1.0)));
    expected.steps.push_back(step(clock_tag::ui, 1, scalar(2.0)));

    run actual;
    actual.steps.push_back(step(clock_tag::ui, 0, scalar(1.0)));

    const auto diff { compare(expected, actual) };

    REQUIRE_FALSE(diff.equal);
    REQUIRE(diff.first_step == 1);
    REQUIRE(diff.output_mismatches == 1);
    REQUIRE(diff.summary.find("steps 2 vs 1") != std::string::npos);
}

TEST_CASE("replay | Trace kind differences are counted", "[replay][compare]")
{
    run expected;
    expected.steps.push_back(step(clock_tag::ui, 0, scalar(1.0)));
    expected.trace.push_back(event(3, 1.0, 1, 20));

    run actual { expected };
    actual.trace[0].kind = kernel::trace_event_kind::region_step_end;

    const auto diff { compare(expected, actual) };

    REQUIRE_FALSE(diff.equal);
    REQUIRE(diff.trace_mismatches == 1);
}
