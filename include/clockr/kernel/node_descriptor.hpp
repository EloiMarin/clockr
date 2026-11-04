#pragma once

#include "clockr/kernel/node.hpp"
#include "clockr/kernel/value.hpp"

#include <span>

namespace clockr::kernel
{

/// The plain-data form of one executable node: id, clock, kind and a tick
/// function representable in a constant. This is what a `program` copies in
/// from a recipe or a `descriptor_view`; evaluation never needs the shell.
struct node_descriptor
{
    // A tick representable in a constant: a function pointer over constant
    // bytes. `self` points into the node's constant state (may be null for
    // stateless ticks); each tick recovers its own state layout from it. The
    // context carries the evaluating region's value home (for constructing
    // user values); scalar ticks ignore it.
    using tick_fn = value (*)(const void* self, const tick_context& ctx, std::span<const value> inputs);

    // Materializes a delay/scan seed the recipe stores by plain value
    // (`closure_cell`) into a kernel value: called once per program, at
    // construction, with the owning region's home (null on the dev path).
    // Null means `self` already points at a `kernel::value` seed (scalars, and
    // every non-delay node).
    using materialize_fn = value (*)(const void* self, const value_home* home);

    node_id id;
    clock_tag clock;
    node_kind kind;
    tick_fn tick;
    const void* self;
    materialize_fn materialize { nullptr };
};

/// The node's dense id.
constexpr
node_id
id(const node_descriptor& node) noexcept
{
    return node.id;
}

/// The node's structural kind (input, output, internal or delay).
constexpr
node_kind
kind(const node_descriptor& node) noexcept
{
    return node.kind;
}

/// The region (clock) the node belongs to.
constexpr
clock_tag
clock(const node_descriptor& node) noexcept
{
    return node.clock;
}

/// Evaluate the node once with its fan-in values. `ctx` carries the evaluating
/// region's value home so user-value ticks can allocate from its pool.
constexpr
value
tick(const node_descriptor& node, const tick_context& ctx, std::span<const value> inputs)
{
    return node.tick(node.self, ctx, inputs);
}

} // namespace clockr::kernel
