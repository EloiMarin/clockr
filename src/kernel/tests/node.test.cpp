#include "clockr/shell/recipe.hpp"
#include "clockr/kernel/node.hpp"
#include "clockr/kernel/node_descriptor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("kernel/node | The four tick families match their std::function twins over a range of inputs", "[kernel]")
{
    // (A standalone constexpr-vs-runtime passthrough smoke used to stand
    // here; the table below sweeps passthrough over three inputs, and the
    // constant-expression case owns the static_asserts.)
    using namespace clockr;
    using namespace clockr::kernel;

    // pass-through: forward the single input unchanged (no state)
    {
        node_descriptor::tick_fn fn = [](const void*, const tick_context&, std::span<const value> in) { return in[0]; };
        node_descriptor compiled { 0, clock_tag::ui, node_kind::internal, fn, nullptr };

        node dev { 0, clock_tag::ui, node_kind::internal, [](const tick_context&, std::span<const value> in) { return in[0]; } };

        for (const value in : { value { std::monostate {} }, value { std::int64_t { 2 } }, value { std::int64_t { -7 } } })
        {
            const std::array<value, 1> one { in };
            REQUIRE(tick(compiled, tick_context{}, one) == tick(dev, tick_context{}, one));
        }
    }

    // constant-generator: emit the value held in state, ignore the input
    {
        constexpr struct gen_state { std::int64_t v; } state { 7 };
        node_descriptor::tick_fn fn = [](const void* self, const tick_context&, std::span<const value>)
        {
            return shell::to_value(static_cast<const gen_state*>(self)->v);
        };
        node_descriptor compiled { 0, clock_tag::ui, node_kind::internal, fn, &state };

        node dev { 0, clock_tag::ui, node_kind::internal, [](const tick_context&, std::span<const value>) { return shell::to_value(std::int64_t { 7 }); } };

        for (const value in : { value { std::monostate {} }, value { std::int64_t { 2 } }, value { std::int64_t { -7 } } })
        {
            const std::array<value, 1> one { in };
            REQUIRE(tick(compiled, tick_context{}, one) == tick(dev, tick_context{}, one));
        }
    }

    // scaling arithmetic: multiply the input by the factor in state
    {
        constexpr struct scale_state { double k; } state { 0.5 };
        node_descriptor::tick_fn fn = [](const void* self, const tick_context&, std::span<const value> in)
        {
            return shell::to_value(shell::from_value<double>(in[0]) * static_cast<const scale_state*>(self)->k);
        };
        node_descriptor compiled { 0, clock_tag::ui, node_kind::internal, fn, &state };

        node dev { 0, clock_tag::ui, node_kind::internal, [](const tick_context&, std::span<const value> in) { return shell::to_value(shell::from_value<double>(in[0]) * 0.5); } };

        for (const value in : { value { 0.0 }, value { 1.0 }, value { 2.5 }, value { -4.0 } })
        {
            const std::array<value, 1> one { in };
            REQUIRE(tick(compiled, tick_context{}, one) == tick(dev, tick_context{}, one));
        }
    }

    // delay-with-init: while the input carries no signal (unit), hold the
    // init from state; once a signal flows, pass it through
    {
        constexpr struct delay_state { std::int64_t init; } state { 42 };
        node_descriptor::tick_fn fn = [](const void* self, const tick_context&, std::span<const value> in)
        {
            return std::holds_alternative<std::monostate>(in[0])
                ? shell::to_value(
                    (*static_cast<const delay_state*>(self)).init
                )
                : in[0];
        };
        node_descriptor compiled { 0, clock_tag::ui, node_kind::internal, fn, &state };

        node dev
        {
            0,
            clock_tag::ui,
            node_kind::internal,
            [](const tick_context&, std::span<const value> in)
            {
                return std::holds_alternative<std::monostate>(in[0]) ? shell::to_value(std::int64_t { 42 }) : in[0];
            }
        };

        for (const value in : { value { std::monostate {} }, value { std::int64_t { 2 } }, value { std::int64_t { -7 } } })
        {
            const std::array<value, 1> one { in };
            REQUIRE(tick(compiled, tick_context{}, one) == tick(dev, tick_context{}, one));
        }
    }
}

TEST_CASE("kernel/node | Stateless and state-by-name ticks evaluate as constant expressions", "[kernel]")
{
    using namespace clockr;
    using namespace clockr::kernel;

    // Stateless: self == nullptr, the tick reads nothing but the inputs.
    constexpr node_descriptor::tick_fn passthrough = [](const void*, const tick_context&, std::span<const value> in) { return in[0]; };
    constexpr node_descriptor stateless { 0, clock_tag::ui, node_kind::internal, passthrough, nullptr };
    constexpr std::array<value, 1> pass_in { std::int64_t { 3 } };

    static_assert(tick(stateless, tick_context{}, pass_in) == value { std::int64_t { 3 } }, "a stateless boxed tick is constant-evaluable");
    REQUIRE(tick(stateless, tick_context{}, pass_in) == value { std::int64_t { 3 } });

    // Stateful, with the tick reaching its state *by name*: recovering the
    // state through self (void* -> T*) is only a constant expression from
    // C++26 (rule 7), so the self-indirection itself
    // is exercised at runtime; the constant-expression guarantee covers the
    // state struct being a constant and the by-name read.
    static constexpr struct gen_state { std::int64_t v; } generator_state { 7 };
    constexpr node_descriptor::tick_fn generator { [](const void*, const tick_context&, std::span<const value>) { return shell::to_value(generator_state.v); } };
    constexpr node_descriptor stateful { 0, clock_tag::ui, node_kind::internal, generator, &generator_state };
    constexpr std::array<value, 1> gen_in { std::int64_t { 9 } };

    static_assert(tick(stateful, tick_context{}, gen_in) == value { std::int64_t { 7 } }, "a stateful boxed tick reaches constant state by name");
    REQUIRE(tick(stateful, tick_context{}, gen_in) == value { std::int64_t { 7 } });
}
