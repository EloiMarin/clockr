#pragma once

#include "clockr/kernel/clock.hpp"
#include "clockr/kernel/value.hpp"

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>

namespace clockr::kernel
{

/// A node's dense, zero-based identity in a program (also its index into the
/// per-node value array).
using node_id = std::uint64_t;

/// The structural role of a node. Boundaries are `input`/`output`; `delay` is
/// a topological seed; everything else is `internal`.
enum class node_kind
{
    input,
    output,
    internal,
    delay,
};

/// The execution context handed to every tick function.
///
/// Today it carries the region's value home: a tick that constructs a user
/// value must allocate from the region that is evaluating, and the engine is
/// the only thing that knows which one that is. It is passed explicitly rather
/// than kept ambient (a thread-local "current pool"): a hidden allocator is the
/// same smell as the ambient thread tag this design deliberately does not have.
struct tick_context
{
    const value_home* home { nullptr };
};

/// The erased-node interface: id, kind, clock and a tick over plain values.
template <class node_t>
concept node_c =
    requires (const node_t& node, const tick_context& ctx, std::span<const kernel::value> inputs)
    {
        { id(node) } -> std::same_as<node_id>;
        { kind(node) } -> std::same_as<kernel::node_kind>;
        { clock(node) } -> std::same_as<clock_tag>;
        { tick(node, ctx, inputs) } -> std::same_as<kernel::value>;
    };

/// The mutable structural node used by the `kernel::graph` builder: an id, a
/// clock, a kind and a `std::function` tick. Compiled programs store the
/// allocation-free `node_descriptor` instead.
class node
{
public:
    using tick_fn = std::function<value(const tick_context&, std::span<const value>)>;

    /// Build a node with the given identity and tick.
    node(node_id id, clock_tag clock, node_kind kind, tick_fn tick) :
        m_id { id },
        m_clock { clock },
        m_kind { kind },
        m_tick { std::move(tick) }
    {}

    friend
    node_id
    id(const node& node) noexcept
    {
        return node.m_id;
    }

    friend
    node_kind
    kind(const node& node) noexcept
    {
        return node.m_kind;
    }

    friend
    clock_tag
    clock(const node& node) noexcept
    {
        return node.m_clock;
    }

    friend
    value
    tick(const node& node, const tick_context& ctx, std::span<const value> inputs)
    {
        return node.m_tick(ctx, inputs);
    }

private:
    node_id m_id;
    clock_tag m_clock;
    node_kind m_kind;
    tick_fn m_tick;
};

} // namespace clockr::kernel
