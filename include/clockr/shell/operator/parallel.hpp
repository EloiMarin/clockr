#pragma once

// parallel: Instantiate N linear chains with N independent outputs.

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

#include <utility>

namespace clockr::shell
{

namespace
{

// Concatenate any number of arenas (one per branch) into one flat array. The
// per-arena sizes are deduced from the array arguments, so no runtime size is
// needed. Each subsequent arena is shifted by the running offset so its
// `node_ref` children keep pointing at the right nodes.
template<std::size_t... Ns>
constexpr
std::array<node, (Ns + ...)>
concat_arenas(const std::array<node, Ns>&... arenas);

}

/// Run independent chains side by side, one output per branch. `parallel` is
/// disjoint composition, not a fan-in: each branch seeds its own clock from
/// its own ports, and the result is an output *span*.
template<recipe_c... Args>
requires (sizeof...(Args) > 0)
constexpr
recipe<void, void, skeleton::concat_t<typename std::remove_cvref_t<Args>::ast...>, (std::remove_cvref_t<Args>::node_count + ...) + 1>
parallel(Args&&... args)
{
    auto combined { concat_arenas(args.storage()...) };

    std::array<node_ref, max_branches> kids {};
    std::size_t offset { 0 };
    std::size_t index { 0 };

    (
        (
            kids[index++] = args.root_index() + offset,
            offset += args.node_count
        ),
        ...
    );

    auto arena { detail::append_node(std::move(combined), node { parallel_t { kids, sizeof...(Args) } }) };

    return recipe<void, void, skeleton::concat_t<typename std::remove_cvref_t<Args>::ast...>, (std::remove_cvref_t<Args>::node_count + ...) + 1>
    {
        std::move(arena),
        (std::remove_cvref_t<Args>::node_count + ...)
    };
}

namespace
{

template<std::size_t... Ns>
constexpr
std::array<node, (Ns + ...)>
concat_arenas(const std::array<node, Ns>&... arenas)
{
    std::array<node, (Ns + ...)> result {};
    std::size_t offset { 0 };

    auto copy
    {
        [&](const auto& a)
        {
            for (std::size_t i { 0 }; i < a.size(); ++i)
                result[offset + i] = detail::shift_indices(a[i], offset);
            offset += a.size();
        }
    };

    (copy(arenas), ...);

    return result;
}

}

} // namespace clockr::shell
