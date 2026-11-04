#pragma once

// zip: N branches (N >= 2), one shared upstream; the output type nests pairs
// left-associatively (zip(a,b,c) : pair<pair<A,B>,C>).
// The pair is compile-time only, it never flows as a kernel::value —
// downstream lift over a pair wire fans out to one kernel edge per leaf.

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

#include <utility>

namespace clockr::shell
{

namespace detail
{

// Left-nested pair type matching left-to-right branch order, so the shell
// type mirrors exactly the id span the walker concatenates.
template<typename... Ins>
struct zip_pair;

template<typename A, typename B>
struct zip_pair<A, B>
{
    using type = std::pair<A, B>;
};

template<typename A, typename B, typename C, typename... Rest>
struct zip_pair<A, B, C, Rest...> : zip_pair<std::pair<A, B>, C, Rest...>
{};

template<typename... Ins>
using zip_pair_t = typename zip_pair<Ins...>::type;

} // namespace detail

/// Combine N ≥ 2 branches of one upstream into a left-nested pair wire
/// (`pair<pair<A,B>,C>`). The pair is a compile-time phantom; a downstream
/// `lift` fans it out to one kernel edge per leaf in zip order (law L8).
template<typename X, typename... Ins, typename... Asts, std::size_t... Ns>
requires (sizeof...(Ins) >= 2)
constexpr
recipe<X, detail::zip_pair_t<Ins...>, skeleton::concat_t<Asts...>, (Ns + ... + 1)>
zip(const recipe<X, Ins, Asts, Ns>&... branches)
{
    static_assert(sizeof...(Ins) <= max_branches, "zip exceeds the branch limit — see max_branches");

    auto combined { detail::concat_arenas(branches.storage()...) };

    std::array<node_ref, max_branches> kids {};
    std::size_t offset { 0 };
    std::size_t index { 0 };

    (
        (
            kids[index++] = branches.root_index() + offset,
            offset += branches.node_count
        ),
        ...
    );

    auto arena { detail::append_node(std::move(combined), node { zip_t { kids, sizeof...(Ins) } }) };

    return recipe<X, detail::zip_pair_t<Ins...>, skeleton::concat_t<Asts...>, (Ns + ... + 1)>
    {
        std::move(arena),
        (Ns + ...)
    };
}

} // namespace clockr::shell
