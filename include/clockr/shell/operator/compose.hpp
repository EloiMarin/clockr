#pragma once

// Composition `lhs >> rhs`: the output of `lhs` feeds the input of `rhs`.
// The middle types must match.

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

#include <concepts>

namespace clockr::shell
{

/// Compose two networks: the output of `lhs` feeds the input of `rhs`. The
/// middle types must match exactly; a mismatch selects the diagnostic
/// overload below, which reports the offending types in a `static_assert`.
template<typename in_t, typename middle_t, typename out_t, typename... L, typename... R, std::size_t NA, std::size_t NB>
constexpr
recipe<in_t, out_t, skeleton::tag_list<L..., R...>, NA + NB + 1>
operator>>(const recipe<in_t, middle_t, skeleton::tag_list<L...>, NA>& lhs, const recipe<middle_t, out_t, skeleton::tag_list<R...>, NB>& rhs)
{
    auto combined { detail::concat2(lhs.storage(), rhs.storage()) };

    auto arena
    {
        detail::append_node(
            std::move(combined),
            node { compose_t(lhs.root_index(), lhs.node_count + rhs.root_index()) }
        )
    };

    return recipe<in_t, out_t, skeleton::tag_list<L..., R...>, NA + NB + 1>
    {
        std::move(arena),
        NA + NB
    };
}

// Diagnostic-only overload: viable only when the middle types disagree, so a
// chain mismatch reports a readable sentence instead of a deduction dump.
template<typename in_t, typename middle1_t, typename middle2_t, typename out_t, typename... L, typename... R>
requires (!std::same_as<middle1_t, middle2_t>)
recipe<in_t, out_t, skeleton::tag_list<L..., R...>>
operator>>(recipe<in_t, middle1_t, skeleton::tag_list<L...>>, recipe<middle2_t, out_t, skeleton::tag_list<R...>>)
{
    static_assert(
        skeleton::always_false<middle1_t, middle2_t>,
        "operator>>: the lhs's output type does not match the rhs's input type — check the chain's middle type");

    return {};
}

} // namespace clockr::shell
