#pragma once

// shell::valid — a constexpr validity predicate for recipe networks.
//
// Returns a `validity` enum rather than throwing, so the same static checks
// that build() performs at runtime can be asserted at compile time with
// `static_assert(shell::valid(net) == shell::validity::ok)`. In a constexpr context
// exceptions are not available, so this is the pre-flight check that
// shell::compile relies on.

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace clockr::shell
{

/// The static well-formedness classes `valid()` reports. Returned instead of
/// thrown so the same checks that `build()` performs can be asserted in a
/// `constexpr` context (where exceptions are unavailable).
enum class validity
{
    ok,
    no_anchor,        // fragment has no input port to derive a clock from
    cross_clock,      // a port's thread disagrees with the derived clock
    duplicate_input,  // two input ports share the same name
    duplicate_output, // two output ports share the same name
    cycle,            // the graph contains a delay-free cycle
    same_clock,       // an at operator tags the same clock as its own context
};

/// A human-readable spelling of a `validity` value, for diagnostics.
constexpr
std::string_view
to_string(validity v) noexcept
{
    switch (v)
    {
        case validity::ok:               return "ok";
        case validity::no_anchor:        return "no anchor";
        case validity::cross_clock:      return "cross-clock flow";
        case validity::duplicate_input:  return "duplicate input port";
        case validity::duplicate_output: return "duplicate output port";
        case validity::cycle:            return "delay-free cycle";
        case validity::same_clock:       return "same clock";
        default:                         return "unknown";
    }
}

namespace detail
{

// Lightweight constexpr walk state. It does not build a full graph; it only
// collects enough information to diagnose the static error classes that
// build() would throw for at runtime.
//
// TODO(chunk G, full constexpr): once the AST is constexpr-constructible and
// the draft uses flat arrays instead of std::map/std::unordered_map, this
// validator can be tightened to also assert exact node/edge budgets and to
// run a full Kahn check over the instantiated graph.
struct validation_context
{
    std::vector<std::string_view> input_names;
    std::vector<std::string_view> output_names;
    std::vector<std::pair<std::uint64_t, std::optional<kernel::clock_tag>>> visited_shares;
};

constexpr
validity
add_port_name(validation_context& ctx, std::string_view name, bool is_input)
{
    auto& names { is_input ? ctx.input_names : ctx.output_names };

    for (const auto existing : names)
    {
        if (existing == name)
            return is_input ? validity::duplicate_input : validity::duplicate_output;
    }

    names.push_back(name);
    return validity::ok;
}

constexpr
bool
already_visited(const validation_context& ctx, std::uint64_t share_id, std::optional<kernel::clock_tag> clock)
{
    for (const auto& [id, c] : ctx.visited_shares)
    {
        if (id == share_id && c == clock)
            return true;
    }

    return false;
}

// zip/parallel establish no clock of their own: each branch seeds itself from
// its own first anchor (mirroring instantiate, which hands branches the
// incoming clock without seeding one). So the root look-ahead below must not
// run for them, or the first branch's anchor would be imposed on its siblings.
constexpr
bool
is_fanout(const node& root) noexcept
{
    return std::visit(
        [](const auto& n) -> bool
        {
            using T = std::decay_t<decltype(n)>;
            return std::is_same_v<T, zip_t> || std::is_same_v<T, parallel_t>;
        },
        root.v);
}

constexpr
validity
validate_node(validation_context& ctx, const node& root, std::optional<kernel::clock_tag>& clock, std::span<const node> arena)
{
    if (!clock && !is_fanout(root))
    {
        const auto anchor { first_anchor(root, arena) };

        if (anchor.k != anchor::kind::input)
            return validity::no_anchor;

        clock = anchor.tag;
    }

    return std::visit(
        [&](const auto& n) -> validity
        {
            using T = std::decay_t<decltype(n)>;

            if constexpr (std::is_same_v<T, lift_t> || std::is_same_v<T, scan_t> || std::is_same_v<T, delay_t>)
            {
                // Leaves: lift has no ports; scan's delay loop and delay nodes
                // are Kahn seeds and therefore allowed. No structural checks
                // needed here.
                return validity::ok;
            }
            else if constexpr (std::is_same_v<T, at_t>)
            {
                // Mirror at_t::instantiate. The incoming clock is not the
                // source's: when the crossing heads a fragment, the seed at
                // the top of this function came from at_t::first_anchor, which
                // reports the *target*. So save nothing, re-seed from the
                // source, and require the source's own clock to differ from
                // the target before re-anchoring downstream on the target.
                clock = std::nullopt;

                const auto source_result { validate_node(ctx, arena[n.source()], clock, arena) };

                if (source_result != validity::ok)
                    return source_result;

                // A successful validate_node always leaves a clock set (it
                // seeds one or reports no_anchor), so this is the source's.
                if (clock == n.clock())
                    return validity::same_clock;

                clock = n.clock();
                return validity::ok;
            }
            else if constexpr (std::is_same_v<T, compose_t>)
            {
                const auto first_result { validate_node(ctx, arena[n.first()], clock, arena) };

                if (first_result != validity::ok)
                    return first_result;

                return validate_node(ctx, arena[n.second()], clock, arena);
            }
            else if constexpr (std::is_same_v<T, input_t>)
            {
                if (clock != n.clock())
                    return validity::cross_clock;

                return add_port_name(ctx, n.port_name(), true);
            }
            else if constexpr (std::is_same_v<T, output_t>)
            {
                if (clock != n.clock())
                    return validity::cross_clock;

                return add_port_name(ctx, n.port_name(), false);
            }
            else if constexpr (std::is_same_v<T, share_t>)
            {
                const auto key { n.id() };

                if (already_visited(ctx, key, clock))
                    return validity::ok;

                ctx.visited_shares.emplace_back(key, clock);
                return validate_node(ctx, arena[n.inner()], clock, arena);
            }
            else if constexpr (std::is_same_v<T, zip_t> || std::is_same_v<T, parallel_t>)
            {
                for (const auto child : n.networks())
                {
                    auto branch_clock { clock };
                    const auto branch_result { validate_node(ctx, arena[child], branch_clock, arena) };

                    if (branch_result != validity::ok)
                        return branch_result;
                }

                return validity::ok;
            }
        },
        root.v
    );
}

} // namespace detail

/// Check a recipe's static well-formedness without building it: anchors,
/// cross-clock flow, duplicate port names, delay-free cycles and same-clock
/// `at<>` annotations. `static_assert(valid(net) == validity::ok)` is the
/// compile-time pre-flight that `shell::compile` relies on.
template<typename in_t, typename out_t, typename skeleton_t, std::size_t N>
constexpr
validity
valid(recipe<in_t, out_t, skeleton_t, N> network)
{
    detail::validation_context ctx;
    std::optional<kernel::clock_tag> clock;

    return detail::validate_node(ctx, network.root(), clock, network.arena());
}

} // namespace clockr::shell
