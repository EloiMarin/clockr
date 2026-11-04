#pragma once

// delay: a unit delay seeded with an initial value. Outputs the seed on the
// first tick, then the previous tick's input — the state half of scan,
// exposed standalone. Feedback loops must pass through a delay (or scan),
// which is what keeps their cycles guarded (law L7).

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

namespace clockr::shell
{

/// A unit delay seeded with `init_value`: emit the seed on the first tick,
/// then the previous tick's input. A feedback cycle must pass through a delay
/// (or scan) to be guarded (law L7).
template<typename T>
constexpr
recipe<T, T, skeleton::tag_list<skeleton::delay_t>, 1>
delay(T init_value)
{
    return recipe<T, T, skeleton::tag_list<skeleton::delay_t>, 1>
    {
        std::array<node, 1>
        {
            node
            {
                delay_t
                {
                    detail::make_init_storage(std::move(init_value)),
                    detail::materialize_for<T>(),
                    kernel::type_tag_or_null<T>()
                }
            }
        },
        0
    };
}

} // namespace clockr::shell
