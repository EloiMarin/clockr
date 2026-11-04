#pragma once

// scan: Accumulates state across ticks.

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace clockr::shell
{

/// A step function for `scan`: const-callable as `(state, input) -> state`.
template<typename fn_t, typename in_t, typename state_t>
concept scan_callable_c =
    std::regular_invocable<const fn_t, const state_t&, const in_t&> &&
    std::convertible_to<std::invoke_result_t<const fn_t, const state_t&, const in_t&>, state_t>;

/// True when `fn_t` is an empty, default-constructible functor: its tick can
/// be reconstructed from the type alone, so the node stays constant data.
template<typename fn_t, typename in1_t, typename in2_t, typename out_t>
constexpr bool stateless_scan_v =
    std::is_convertible_v<fn_t, out_t(*)(const in1_t&, const in2_t&)> &&
    std::is_empty_v<fn_t> &&
    std::is_default_constructible_v<fn_t>;

/// Stateful fold: `state(t) = tick_fn(state(t-1), input(t))`. The seed is
/// emitted before any input arrives; the new state is settled after each
/// tick and consumed on the next one.
template<typename in_t, typename state_t, typename fn_t>
requires scan_callable_c<fn_t, in_t, state_t>
constexpr
recipe<in_t, state_t, skeleton::tag_list<skeleton::scan_t>, 1>
scan(state_t init_state, fn_t tick_fn)
{
    if constexpr (stateless_scan_v<fn_t, state_t, in_t, state_t>)
    {
        return recipe<in_t, state_t, skeleton::tag_list<skeleton::scan_t>, 1>
        {
            std::array<node, 1>
            {
                node
                {
                    scan_t(
                        detail::make_init_storage(std::move(init_state)),
                        detail::materialize_for<state_t>(),
                        &detail::stateless_tick2<fn_t, state_t, in_t, state_t>,
                        detail::closure_cell{},
                        kernel::type_tag_or_null<state_t>()
                    )
                }
            },
            0
        };
    }
    else
    {
        return recipe<in_t, state_t, skeleton::tag_list<skeleton::scan_t>, 1>
        {
            std::array<node, 1>
            {
                node
                {
                    scan_t(
                        detail::make_init_storage(std::move(init_state)),
                        detail::materialize_for<state_t>(),
                        &detail::boxed_tick2<fn_t, state_t, in_t, state_t>,
                        detail::closure_cell{std::move(tick_fn)},
                        kernel::type_tag_or_null<state_t>()
                    )
                }
            },
            0
        };
    }
}

} // namespace clockr::shell
