#pragma once

/// @file
/// A `scan` whose seed is shared rather than copied into the recipe node.
///
/// `shell::scan` stores its seed inline in the node's 64-byte closure cell, so
/// a large user state (`jam::score` is ~1.5 KiB) cannot be a seed directly.
/// This operator stores an immutable `shared_ptr<const State>` in the closure
/// instead and materializes a fresh copy into the owning region's pool at
/// program construction — exactly the same construction-time path as any other
/// user-typed seed. Nothing is allocated on a ticking thread, and programs
/// built from the same recipe each get their own state.

#include "clockr/kernel/user_type.hpp"
#include "clockr/shell/recipe.hpp"

#include <array>
#include <concepts>
#include <memory>
#include <utility>

namespace jam
{

namespace detail
{

template<typename T>
struct seed_holder
{
    std::shared_ptr<const T> value;
};

template<typename T>
clockr::kernel::value
materialize_shared_seed(const void* self, const clockr::kernel::value_home* home)
{
    const auto& holder { *static_cast<const seed_holder<T>*>(self) };
    return clockr::kernel::make_user_value<T>(home, *holder.value);
}

} // namespace detail

/// Stateful fold seeded from an immutable shared value: the same contract as
/// `shell::scan`, but the seed may be arbitrarily large.
template<typename in_t, typename state_t, typename fn_t>
requires clockr::shell::scan_callable_c<fn_t, in_t, state_t>
clockr::shell::recipe<
    in_t,
    state_t,
    clockr::shell::skeleton::tag_list<clockr::shell::skeleton::scan_t>,
    1>
scan_seeded(std::shared_ptr<const state_t> init_state, fn_t tick_fn)
{
    using clockr::kernel::type_tag_or_null;
    using clockr::shell::detail::boxed_tick2;
    using clockr::shell::detail::closure_cell;
    using clockr::shell::node;
    using clockr::shell::recipe;
    using scan_node = clockr::shell::scan_t;
    using scan_tag = clockr::shell::skeleton::scan_t;
    using tag_list = clockr::shell::skeleton::tag_list<scan_tag>;

    return recipe<in_t, state_t, tag_list, 1>
    {
        std::array<node, 1>
        {
            node
            {
                scan_node(
                    closure_cell {
                        jam::detail::seed_holder<state_t> { std::move(init_state) } },
                    &jam::detail::materialize_shared_seed<state_t>,
                    &boxed_tick2<fn_t, state_t, in_t, state_t>,
                    closure_cell { std::move(tick_fn) },
                    type_tag_or_null<state_t>()
                )
            }
        },
        0
    };
}

} // namespace jam
