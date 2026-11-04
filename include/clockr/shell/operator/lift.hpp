#pragma once

// lift: a pure function over its inputs becomes a one-node network.
// The input type decides the arity: a plain type is one fan-in edge, a
// (possibly nested) pair is one edge per leaf, left to right — the same
// phantom-pair wiring as zip (L8), generalized from 2 to N. A binary lift is
// spelled `lift<std::pair<A, B>, R>`; there is no separate binary operator.
//
// The operator functions live here; the `lift_t` node they build lives in
// ast.hpp alongside the rest of the AST. recipe.hpp bottom-includes this header so
// that `<recipe/recipe.hpp>` keeps exposing `lift`.

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

namespace clockr::shell
{

/// True for a `std::pair` wire type. Pairs are a compile-time phantom: they
/// describe fan-in arity and never flow as a `kernel::value`.
template<typename T>
struct is_pair : std::false_type {};

template<typename A, typename B>
struct is_pair<std::pair<A, B>> : std::true_type {};

/// Convenience variable template for `is_pair`.
template<typename T>
constexpr bool is_pair_v = is_pair<T>::value;

/// Flatten a nested-pair wire type into its leaf list, left to right — exactly
/// the order in which the walker concatenates fan-in edges for nested zips.
template<typename T>
struct flatten
{
    using type = std::tuple<T>;
};

template<typename A, typename B>
struct flatten<std::pair<A, B>>
{
    using type = decltype(std::tuple_cat(
        std::declval<typename flatten<A>::type>(),
        std::declval<typename flatten<B>::type>()));
};

/// The flat leaf tuple for a wire type.
template<typename T>
using flatten_t = typename flatten<T>::type;

/// A unary function `in_t -> out_t` usable by `lift`.
template<typename fn_t, typename in_t, typename out_t>
concept lift_callable_c =
    std::regular_invocable<const fn_t, const in_t&> &&
    std::convertible_to<std::invoke_result_t<const fn_t, const in_t&>, out_t>;

template<typename fn_t, typename tuple_ins_t, typename out_t>
struct liftN_callable;

template<typename fn_t, typename... ins_t, typename out_t>
struct liftN_callable<fn_t, std::tuple<ins_t...>, out_t> : std::bool_constant<
    std::regular_invocable<const fn_t, const ins_t&...> &&
    std::convertible_to<std::invoke_result_t<const fn_t, const ins_t&...>, out_t>> {};

/// An N-ary function over the flattened leaves, usable by `lift`.
template<typename fn_t, typename tuple_ins_t, typename out_t>
concept liftN_callable_c = liftN_callable<fn_t, tuple_ins_t, out_t>::value;

// A "stateless" fn_t can be turned into a captureless tick, hence a constant.
// Empty closure types (captureless lambdas) qualify; so do empty functors.
// Plain function *pointers* are a separate overload (chunk F/G, NTTP) — a
// default-constructed pointer is null, so they cannot go through fn_t{}.
/// True when a unary `fn_t` is empty and default-constructible, so its tick
/// can be reconstructed from the type alone (constant-initializable).
template<typename fn_t, typename in_t, typename out_t>
constexpr bool stateless_lift_v =
    std::is_convertible_v<fn_t, out_t(*)(const in_t&)> &&
    std::is_empty_v<fn_t> &&
    std::is_default_constructible_v<fn_t>;

template<typename tuple_ins_t, typename out_t>
struct liftN_fn_ptr;

template<typename... ins_t, typename out_t>
struct liftN_fn_ptr<std::tuple<ins_t...>, out_t>
{
    using type = out_t(*)(const ins_t&...);
};

/// True when an N-ary `fn_t` is empty and default-constructible, so its tick
/// can be reconstructed from the type alone (constant-initializable).
template<typename fn_t, typename in_t, typename out_t>
constexpr bool stateless_liftN_v =
    std::is_convertible_v<fn_t, typename liftN_fn_ptr<flatten_t<in_t>, out_t>::type> &&
    std::is_empty_v<fn_t> &&
    std::is_default_constructible_v<fn_t>;

/// Turn a pure unary function into a one-node network. A plain `in_t` is one
/// fan-in edge; a nested pair is one edge per leaf, left to right (law L8).
template<typename in_t, typename out_t, typename fn_t>
requires (!is_pair_v<in_t>) && lift_callable_c<fn_t, in_t, out_t>
constexpr
recipe<in_t, out_t, skeleton::tag_list<skeleton::lift_t>, 1>
lift(fn_t tick_fn)
{
    if constexpr (stateless_lift_v<fn_t, in_t, out_t>)
    {
        return recipe<in_t, out_t, skeleton::tag_list<skeleton::lift_t>, 1>
        {
            std::array<node, 1>
            {
                node
                {
                    lift_t(
                        &detail::stateless_tick<fn_t, in_t, out_t>,
                        detail::closure_cell{},
                        1,
                        kernel::type_tag_or_null<out_t>()
                    )
                }
            },
            0
        };
    }
    else
    {
        return recipe<in_t, out_t, skeleton::tag_list<skeleton::lift_t>, 1>
        {
            std::array<node, 1>
            {
                node
                {
                    lift_t(
                        &detail::boxed_tick<fn_t, in_t, out_t>,
                        detail::closure_cell{std::move(tick_fn)},
                        1,
                        kernel::type_tag_or_null<out_t>()
                    )
                }
            },
            0
        };
    }
}

// lift over an N-ary wire: a nested-pair input (as built by nested zip) is
// one fan-in edge per leaf, left to right — L8 generalized from pairs to
// N-tuples. The pair never flows as a value; each edge converts strictly on
// its own, so totality and the kernel's closed value set are untouched.
/// N-ary `lift`: the pair-wire input is flattened into leaves and each leaf
/// becomes its own fan-in edge, so the function receives one argument per
/// leaf in zip order.
template<typename in_t, typename out_t, typename fn_t>
requires is_pair_v<in_t> && liftN_callable_c<fn_t, flatten_t<in_t>, out_t>
constexpr
recipe<in_t, out_t, skeleton::tag_list<skeleton::liftN_t<std::tuple_size_v<flatten_t<in_t>>>>, 1>
lift(fn_t tick_fn)
{
    constexpr std::size_t arity { std::tuple_size_v<flatten_t<in_t>> };
    using leaves_t = flatten_t<in_t>;

    if constexpr (stateless_liftN_v<fn_t, in_t, out_t>)
    {
        return recipe<in_t, out_t, skeleton::tag_list<skeleton::liftN_t<arity>>, 1>
        {
            std::array<node, 1>
            {
                node
                {
                    lift_t(
                        &detail::stateless_tick_n<fn_t, leaves_t, out_t>,
                        detail::closure_cell{},
                        arity,
                        kernel::type_tag_or_null<out_t>()
                    )
                }
            },
            0
        };
    }
    else
    {
        return recipe<in_t, out_t, skeleton::tag_list<skeleton::liftN_t<arity>>, 1>
        {
            std::array<node, 1>
            {
                node
                {
                    lift_t(
                        &detail::boxed_tick_n<fn_t, leaves_t, out_t>,
                        detail::closure_cell{std::move(tick_fn)},
                        arity,
                        kernel::type_tag_or_null<out_t>()
                    )
                }
            },
            0
        };
    }
}

} // namespace clockr::shell
