#include "clockr/replay/native.hpp"

#include "test_support.hpp"

#include "clockr/kernel/release_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace clockr;
using namespace clockr::replay;

TEST_CASE(
    "replay | The native adapter builds a fresh program per replay",
    "[replay][adapter]"
)
{
    using recipe_t = decltype(replay_test::network());

    replay::native::recipe_factory<recipe_t> factory { replay_test::network() };

    scenario recorded;
    recorded.run = run_id { 1 };
    recorded.branch = branch_id { 1 };

    auto first { factory.make(recorded, {}) };
    auto second { factory.make(recorded, {}) };

    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first.get() != second.get());

    kernel::release_engine engine;

    first->set("level", kernel::value { 4.0 });
    engine.step(*first, kernel::clock_tag::ui);
    engine.step(*first, kernel::clock_tag::audio);

    REQUIRE(std::get<double>(first->get("total")) == 4.0);
    REQUIRE(std::holds_alternative<std::monostate>(second->get("total")));
}
