#pragma once

// stepper: the named absorb pattern — an event crossing made level again.
//
// An optional-typed input fires for exactly one tick (fire-and-forget); the
// level-like consumers downstream need the last firing held instead. This is
// scan(init, value_or) with the step spelled out, so the persistence shows up
// as an ordinary scan node with a visible seed — never as port behaviour,
// and with no new AST node, walker case, or kernel change.
//
// Keep it off event channels: a held event repeats every silent tick, which
// is precisely what edge-triggered consumers must not see. For those, carry
// the optional to the decision point instead.

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"
#include "clockr/shell/operator/scan.hpp"

#include <optional>
#include <utility>

namespace clockr::shell
{

/// Turn an event (an `optional<T>` input) into a level by carrying its last
/// firing: `scan(init_state, value_or)`. The seed is visible as an ordinary
/// scan node. Do not use it on a channel whose consumer must see each event
/// exactly once — a carried event repeats on every silent tick.
template<typename T>
constexpr
recipe<std::optional<T>, T, skeleton::tag_list<skeleton::scan_t>, 1>
stepper(T init_state)
{
    return scan<std::optional<T>, T>(
        std::move(init_state),
        [](const T& prev, const std::optional<T>& m) { return m.value_or(prev); });
}

} // namespace clockr::shell
