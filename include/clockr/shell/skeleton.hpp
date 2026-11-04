#pragma once

/// @file
/// The compile-time shape of a network, as a flat list of tags.
//
// Only operators that emit graph nodes contribute a tag; compose/share/zip/
// parallel sequence children but emit nothing, so they never appear here. Each
// tag carries the node/edge cost of its operator, and every size a compiled
// program needs is summed from the list — via a constexpr loop over a flat
// brace-init array, so there is no fold-expression nesting limit, no template
// recursion depth, and no hand-guessed capacities.
//
// The list is a multiset: order is irrelevant, only counts matter. It is an
// EXACT count for share-free networks, and a tight UPPER BOUND where `share`
// deduplicates a fragment used more than once — dedup keys on the share node's
// runtime identity (a value-level fact), which no type can observe, so a shared
// fragment used N times contributes N tags but emits once.

#include <cstddef>
#include <type_traits>

namespace clockr::shell::skeleton
{

struct input_t
{
    static constexpr std::size_t nodes { 1 };
    static constexpr std::size_t edges { 0 };
};

struct lift_t
{
    static constexpr std::size_t nodes { 1 };
    static constexpr std::size_t edges { 1 };
};

template<std::size_t N>
struct liftN_t
{
    static constexpr std::size_t nodes { 1 };
    static constexpr std::size_t edges { N };
};

struct output_t
{
    static constexpr std::size_t nodes { 1 };
    static constexpr std::size_t edges { 1 };
};

struct scan_t
{
    static constexpr std::size_t nodes { 2 };   // delay node + scanner node
    static constexpr std::size_t edges { 3 };   // delay->scanner, in->scanner, scanner->delay
};

struct delay_t
{
    static constexpr std::size_t nodes { 1 };   // the delay node itself
    static constexpr std::size_t edges { 1 };   // in->delay
};

template<typename... Tags>
struct tag_list {};

// --- sizes, all loop sums over flat arrays ---

template<typename Ast>
inline constexpr std::size_t node_count { 0 };

template<typename... Tags>
inline constexpr std::size_t node_count<tag_list<Tags...>> = []() constexpr
{
    constexpr std::size_t counts[] { Tags::nodes... };
    std::size_t total { 0 };

    for (const std::size_t count : counts)
        total += count;

    return total;
}();

template<typename Ast>
inline constexpr std::size_t edge_count { 0 };

template<typename... Tags>
inline constexpr std::size_t edge_count<tag_list<Tags...>> = []() constexpr
{
    constexpr std::size_t counts[] { Tags::edges... };
    std::size_t total { 0 };

    for (const std::size_t count : counts)
        total += count;

    return total;
}();

template<typename T, typename Ast>
inline constexpr std::size_t count_of { 0 };

template<typename T, typename... Tags>
inline constexpr std::size_t count_of<T, tag_list<Tags...>> = []() constexpr
{
    constexpr bool matches[] { std::is_same_v<T, Tags>... };
    std::size_t count { 0 };

    for (const bool match : matches)
        count += match ? 1u : 0u;

    return count;
}();

template<typename Ast>
inline constexpr std::size_t input_count { 0 };

template<typename... Tags>
inline constexpr std::size_t input_count<tag_list<Tags...>> { count_of<input_t, tag_list<Tags...>> };

template<typename Ast>
inline constexpr std::size_t output_count { 0 };

template<typename... Tags>
inline constexpr std::size_t output_count<tag_list<Tags...>> { count_of<output_t, tag_list<Tags...>> };

template<typename Ast>
inline constexpr std::size_t scan_count { 0 };

template<typename... Tags>
inline constexpr std::size_t scan_count<tag_list<Tags...>> { count_of<scan_t, tag_list<Tags...>> };

template<typename Ast>
inline constexpr std::size_t delay_count { 0 };

template<typename... Tags>
inline constexpr std::size_t delay_count<tag_list<Tags...>> { count_of<delay_t, tag_list<Tags...>> };

// --- flatten several tag_lists into one (depth = number of branches, not nodes) ---

template<typename...>
struct concat;

template<>
struct concat<>
{
    using type = tag_list<>;
};

template<typename... Tags>
struct concat<tag_list<Tags...>>
{
    using type = tag_list<Tags...>;
};

template<typename... Ls, typename... Rs, typename... Rest>
struct concat<tag_list<Ls...>, tag_list<Rs...>, Rest...> :
    concat<tag_list<Ls..., Rs...>, Rest...>
{};

template<typename... Asts>
using concat_t = typename concat<Asts...>::type;

// For readable static_asserts: a dependent-false value naming the offending types.
template<typename...>
inline constexpr bool always_false { false };

} // namespace clockr::shell::skeleton
