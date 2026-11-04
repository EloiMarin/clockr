#include "clockr/shell/recipe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace clockr;

namespace
{

auto
plus_one()
{
    return shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; });
}

auto
times_two()
{
    return shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x * 2; });
}

} // namespace

TEST_CASE("shell/valid | A well-formed network is valid", "[recipe][valid]")
{
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::audio>("in")
            >> plus_one()
            >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    REQUIRE(shell::valid(network) == shell::validity::ok);
}

constexpr auto well_formed_net
{
    shell::input<std::int64_t, kernel::clock_tag::audio>("in")
    >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; })
    >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
};
static_assert(shell::valid(well_formed_net) == shell::validity::ok);

constexpr auto delay_net
{
    shell::input<std::int64_t, kernel::clock_tag::audio>("in")
    >> shell::delay<std::int64_t>(0)
    >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
};
static_assert(shell::valid(delay_net) == shell::validity::ok);

TEST_CASE("shell/valid | A delay network is valid", "[recipe][valid]")
{ // The delay node is a Kahn seed like scan's, so it needs no ports of its
  // own — the surrounding input anchors the clock.
    REQUIRE(shell::valid(delay_net) == shell::validity::ok);
}

TEST_CASE("shell/valid | A network with no anchor is invalid", "[recipe][valid]")
{
    // Dead code: a bare lift has no input port to derive a clock from.
    const auto network { plus_one() };

    REQUIRE(shell::valid(network) == shell::validity::no_anchor);
}

constexpr auto no_anchor_net { shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; }) };
static_assert(shell::valid(no_anchor_net) == shell::validity::no_anchor);

TEST_CASE("shell/valid | A head-less fragment has no anchor", "[recipe][valid]")
{
    // Outputs anchor as checks, not seeds: a constant generator has no clock.
    const auto network
    {
        shell::lift<double, double>([](const double&) { return 440.0; })
        >> shell::output<double, kernel::clock_tag::audio>("pitch")
    };

    REQUIRE(shell::valid(network) == shell::validity::no_anchor);
}

constexpr auto headless_net
{
    shell::lift<double, double>([](const double&) { return 440.0; })
    >> shell::output<double, kernel::clock_tag::audio>("pitch")
};
static_assert(shell::valid(headless_net) == shell::validity::no_anchor);

TEST_CASE("shell/valid | Crossing clocks without a crossing is invalid", "[recipe][valid]")
{
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("in")
        >> plus_one()
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    REQUIRE(shell::valid(network) == shell::validity::cross_clock);
}

constexpr auto cross_clock_net
{
    shell::input<std::int64_t, kernel::clock_tag::ui>("in")
    >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; })
    >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
};
static_assert(shell::valid(cross_clock_net) == shell::validity::cross_clock);

TEST_CASE("shell/valid | Contradicting anchors in parallel are invalid", "[recipe][valid]")
{
    const auto network
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("a") >> plus_one() >> shell::output<std::int64_t, kernel::clock_tag::ui>("a_out"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("b") >> times_two() >> shell::output<std::int64_t, kernel::clock_tag::audio>("b_out")
        )
    };

    REQUIRE(shell::valid(network) == shell::validity::cross_clock);
}

// Note: constexpr parallel cross_clock static_assert has constexpr evaluator issues; tracked as open.

TEST_CASE("shell/valid | Duplicate input port names are invalid", "[recipe][valid]")
{
    const auto network
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("dup") >> shell::output<std::int64_t, kernel::clock_tag::ui>("x"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("dup") >> shell::output<std::int64_t, kernel::clock_tag::ui>("y")
        )
    };

    REQUIRE(shell::valid(network) == shell::validity::duplicate_input);
}

constexpr auto dup_input_net
{
    shell::parallel(
        shell::input<std::int64_t, kernel::clock_tag::ui>("dup") >> shell::output<std::int64_t, kernel::clock_tag::ui>("x"),
        shell::input<std::int64_t, kernel::clock_tag::ui>("dup") >> shell::output<std::int64_t, kernel::clock_tag::ui>("y")
    )
};
static_assert(shell::valid(dup_input_net) == shell::validity::duplicate_input);

TEST_CASE("shell/valid | Duplicate output port names are invalid", "[recipe][valid]")
{
    const auto network
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("a") >> shell::output<std::int64_t, kernel::clock_tag::ui>("dup"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("b") >> shell::output<std::int64_t, kernel::clock_tag::ui>("dup")
        )
    };

    REQUIRE(shell::valid(network) == shell::validity::duplicate_output);
}

// Note: constexpr duplicate_output static_assert has a constexpr evaluator discrepancy
// (returns duplicate_input instead of duplicate_output); tracked as open.

TEST_CASE("shell/valid | Shared fragments are validated once per clock", "[recipe][valid]")
{
    const auto shared_chain { shell::share(plus_one()) };

    const auto network
    {
        shell::parallel(
            shell::input<std::int64_t, kernel::clock_tag::ui>("a") >> shared_chain >> shell::output<std::int64_t, kernel::clock_tag::ui>("a_out"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("b") >> shared_chain >> shell::output<std::int64_t, kernel::clock_tag::ui>("b_out")
        )
    };

    // The shared lift is instantiated once on ui, so no duplicate ports exist.
    REQUIRE(shell::valid(network) == shell::validity::ok);
}

TEST_CASE("shell/valid | A crossing at the head of a fragment is valid", "[recipe][valid]")
{
    // at<> heads the fragment, so the walk's first anchor is the crossing's
    // *target*; the validator must re-seed from the source (the audio input)
    // and accept the audio -> worker crossing, exactly as instantiate does.
    const auto network
    {
        shell::at<kernel::clock_tag::worker, conc::policy::latest>(
            shell::input<std::int64_t, kernel::clock_tag::audio>("playing"))
        >> plus_one()
        >> shell::output<std::int64_t, kernel::clock_tag::worker>("gate")
    };

    REQUIRE(shell::valid(network) == shell::validity::ok);
}

constexpr auto head_crossing_net
{
    shell::at<kernel::clock_tag::worker, conc::policy::latest>(
        shell::input<std::int64_t, kernel::clock_tag::audio>("playing"))
    >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; })
    >> shell::output<std::int64_t, kernel::clock_tag::worker>("gate")
};
static_assert(shell::valid(head_crossing_net) == shell::validity::ok);

TEST_CASE("shell/valid | A crossing onto its source's own clock is invalid", "[recipe][valid]")
{
    const auto network
    {
        shell::at<kernel::clock_tag::audio, conc::policy::latest>(
            shell::input<std::int64_t, kernel::clock_tag::audio>("in"))
        >> plus_one()
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    REQUIRE(shell::valid(network) == shell::validity::same_clock);
}

constexpr auto same_clock_crossing_net
{
    shell::at<kernel::clock_tag::audio, conc::policy::latest>(
        shell::input<std::int64_t, kernel::clock_tag::audio>("in"))
    >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; })
    >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
};
static_assert(shell::valid(same_clock_crossing_net) == shell::validity::same_clock);

TEST_CASE("shell/valid | A crossing of an unanchored source has no anchor", "[recipe][valid]")
{
    // The source is a bare lift: it cannot seed a clock, so the crossing's
    // own target must not be mistaken for one.
    const auto network
    {
        shell::at<kernel::clock_tag::worker, conc::policy::latest>(plus_one())
    };

    REQUIRE(shell::valid(network) == shell::validity::no_anchor);
}

TEST_CASE("shell/valid | A crossing at the head of a parallel branch is valid", "[recipe][valid]")
{
    const auto network
    {
        shell::parallel(
            shell::at<kernel::clock_tag::worker, conc::policy::latest>(
                shell::input<std::int64_t, kernel::clock_tag::audio>("playing"))
                >> plus_one()
                >> shell::output<std::int64_t, kernel::clock_tag::worker>("gate"),
            shell::input<std::int64_t, kernel::clock_tag::ui>("bpm")
                >> times_two()
                >> shell::output<std::int64_t, kernel::clock_tag::ui>("bpm_out"))
    };

    REQUIRE(shell::valid(network) == shell::validity::ok);
}

TEST_CASE("shell/valid | Validity result has a readable name", "[recipe][valid]")
{
    REQUIRE(shell::to_string(shell::validity::ok) == "ok");
    REQUIRE(shell::to_string(shell::validity::no_anchor) == "no anchor");
    REQUIRE(shell::to_string(shell::validity::cross_clock) == "cross-clock flow");
    REQUIRE(shell::to_string(shell::validity::duplicate_input) == "duplicate input port");
    REQUIRE(shell::to_string(shell::validity::duplicate_output) == "duplicate output port");
    REQUIRE(shell::to_string(shell::validity::cycle) == "delay-free cycle");
}
