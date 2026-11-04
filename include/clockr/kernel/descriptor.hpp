#pragma once

#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/topology.hpp"
#include "clockr/kernel/node_descriptor.hpp"
#include "clockr/kernel/node_label.hpp"

#include <cassert>
#include <span>

namespace clockr::kernel
{

/// A borrowed, immutable description of a compiled program: the plain-data
/// lowering of a recipe (or a hand-written literal in tests). Every span is
/// non-owning and must outlive any `program` built from this view; building
/// copies the data but keeps borrowing closure `self` pointers and port-name
/// bytes.
struct descriptor_view
{
    using node_type = node_descriptor;

    std::span<const node_type>     nodes;
    std::span<const node_label>    labels;          // parallel to nodes, indexed by id
    std::span<const edge>          edges;
    std::span<const node_id>       fan_in;          // flat
    std::span<const std::size_t>   fan_in_offsets;  // node_count+1 prefix sums
    std::span<const node_id>       evaluation_order;
    std::span<const region>        regions;
    std::span<const crossing> crossings;
    std::span<const node_id>       input_ids;
    std::span<const node_id>       output_ids;
    std::span<const char* const>   input_names;
    std::span<const char* const>   output_names;

    // The user value types this network can carry, for the debugger's schema
    // registry (deduplicated, discovery order). Trailing and defaulted so
    // existing positional aggregate descriptors stay valid.
    std::span<const user_type* const> types {};
};

/// Number of nodes in the description.
constexpr
std::size_t
size(const descriptor_view& graph) noexcept
{
    return graph.nodes.size();
}

/// The descriptor node with id `id`.
constexpr
const descriptor_view::node_type&
at(const descriptor_view& graph, node_id id) noexcept
{
    return graph.nodes[id];
}

// The node's descriptive label, or a null label when the descriptor carries
// none (labels are optional — evaluation never reads them). `id` must be a
// valid node id; a descriptor with fewer labels than nodes returns a null
// label rather than reaching out of bounds.
constexpr
node_label
label(const descriptor_view& graph, node_id id) noexcept
{
    return id < graph.labels.size() ? graph.labels[id] : node_label {};
}

/// The fan-in node ids of `id`, in the order edges were connected.
constexpr
std::span<const node_id>
fan_in(const descriptor_view& graph, node_id id) noexcept
{
    const auto fan_in_begin { graph.fan_in_offsets[id] };
    const auto fan_in_end { graph.fan_in_offsets[id + 1] };

    assert(fan_in_end >= fan_in_begin);
    const auto count { fan_in_end - fan_in_begin };

    return graph.fan_in.subspan(fan_in_begin, count);
}

/// All nodes, indexed by id.
constexpr
std::span<const descriptor_view::node_type>
nodes(const descriptor_view& graph) noexcept
{
    return graph.nodes;
}

/// All directed edges.
constexpr
std::span<const edge>
edges(const descriptor_view& graph) noexcept
{
    return graph.edges;
}

} // namespace clockr::kernel
