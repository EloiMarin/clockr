#pragma once

#include "clockr/kernel/node.hpp"

#include <concepts>
#include <cstddef> // std::size_t
#include <span>
#include <utility>
#include <vector>

namespace clockr::shell
{
class draft;
}

namespace clockr::kernel
{

/// A directed edge between two nodes: `from` produces a value consumed by
/// `to`.
struct edge
{
    node_id from;
    node_id to;
};

/// Structural interface shared by the mutable `graph` builder and the
/// immutable `descriptor_view`. Engines and topology only need this much:
/// node count, node lookup, per-node fan-in and the edge list.
template <class graph_t>
concept graph_c =
    node_c<typename graph_t::node_type> &&
    requires (const graph_t& graph, kernel::node_id node_id)
    {
        { size(graph) } -> std::convertible_to<std::size_t>;
        { at(graph, node_id) } -> std::same_as<typename graph_t::node_type const&>;
        { fan_in(graph, node_id) } -> std::same_as<std::span<const kernel::node_id>>;
        { nodes(graph) } -> std::same_as<std::span<const typename graph_t::node_type>>;
        { edges(graph) } -> std::same_as<std::span<const edge>>;
    };

/// The mutable structural builder: add nodes and edges, then derive topology.
/// This is the lowering target of `shell::build`; it is not the runtime
/// execution plane (that is `program`).
class graph
{
public:
    using node_type = node;

    friend
    std::size_t
    size(const graph& graph) noexcept
    {
        return graph.m_nodes.size();
    }

    friend
    const node_type&
    at(const graph& graph, node_id id)
    {
        return graph.m_nodes.at(id);
    }

    friend
    std::span<const node_id>
    fan_in(const graph& graph, node_id id)
    {
        return graph.m_fan_in.at(id);
    }

    friend
    std::span<const node>
    nodes(const graph& graph) noexcept
    {
        return graph.m_nodes;
    }

    friend
    std::span<const edge>
    edges(const graph& graph) noexcept
    {
        return graph.m_edges;
    }

    /// Append a node to `clock` and return its dense id.
    node_id
    add_node(clock_tag clock, node_kind kind, node::tick_fn tick)
    {
        const auto id { m_nodes.size() };

        m_nodes.emplace_back(id, clock, kind, std::move(tick));
        m_fan_in.emplace_back();

        return id;
    }

    /// Append a delay node seeded with `initial_value`. Delay nodes are
    /// topological seeds, so a cycle through one is legal (law L7).
    node_id
    add_delay(clock_tag clock, [[maybe_unused]] value initial_value)
    {
        return add_node(
            clock,
            node_kind::delay,
            [initial_value](const tick_context&, std::span<const value>) { return initial_value; }
        );
    }

    /// Add an edge `from -> to` and record it in `to`'s fan-in.
    void
    connect(node_id from, node_id to)
    {
        m_edges.push_back({ from, to });
        m_fan_in[to].push_back(from);
    }

private:
    std::vector<node> m_nodes;
    std::vector<edge> m_edges;
    std::vector<std::vector<node_id>> m_fan_in;
};

} // namespace clockr::kernel
