#pragma once

// descriptor_storage — the constant (description) plane of a program.
//
// This is what constexpr `shell::compile` returns: arrays sized from the
// network's compile-time skeleton (ast_skeleton.hpp), from which
// a `kernel::descriptor_view` — the span view the engine reads — is derived on
// demand. Sizes are type-derived, never hand-guessed; they are exact for
// share-free networks and a tight upper bound where `share` deduplicates a
// fragment used more than once (see ast_skeleton.hpp).
//
// The mutable execution plane (input/output/delay value buffers) lives outside
// this type, in `kernel::program`, sized from this descriptor at runtime
// (§2's two-plane split: a constexpr object cannot own heap/vectors).
//
// Cell/state data does not live here either: `node_descriptor.self` points into
// the constexpr recipe (the AST node that owns the closure inline), so the
// descriptor contains only plain arrays + spans of scalars. The recipe must
// therefore outlive any `program` built from this descriptor.
//
// Spans are never stored here (a span built against a local then copied would
// dangle); `graph()` derives them fresh from these members each call.

#include "clockr/shell/draft.hpp"
#include "clockr/shell/recipe.hpp"

#include "clockr/kernel/descriptor.hpp"
#include "clockr/kernel/node_descriptor.hpp"
#include "clockr/kernel/topology.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace clockr::shell
{

/// The constant (description) plane of a program: fixed-size arrays sized from
/// the network's compile-time skeleton, from which a `kernel::descriptor_view`
/// is derived on demand. `shell::compile` produces one at compile time; a
/// `kernel::program` built from it borrows the data and the recipe storage
/// referenced by `self` pointers.
template<typename skeleton_t>
struct descriptor_storage
{
    // The compile-time skeleton (ast_skeleton.hpp) sizes the arrays: it is the
    // multiset of graph-emitting operators, so it is an exact count for
    // share-free networks and a tight upper bound where `share` deduplicates a
    // fragment used more than once (dedup is a value-level fact, invisible to
    // types). The exact emitted counts below are filled by the walk.
    // Each scan embeds one delay node and each delay operator is one, so
    // the slot budget is the sum of both counts.
    static constexpr std::size_t delay_slots { skeleton::scan_count<skeleton_t> + skeleton::delay_count<skeleton_t> };

    std::size_t node_count { 0 };
    std::size_t edge_count { 0 };
    std::size_t input_count { 0 };
    std::size_t output_count { 0 };
    std::size_t crossing_count { 0 };

    std::array<kernel::node_descriptor, skeleton::node_count<skeleton_t>> nodes {};
    std::array<kernel::node_label, skeleton::node_count<skeleton_t>> labels {};
    std::array<kernel::edge, skeleton::edge_count<skeleton_t>> edges {};

    std::array<kernel::node_id, skeleton::edge_count<skeleton_t>> fan_in {};
    std::array<std::size_t, skeleton::node_count<skeleton_t> + 1> fan_in_offsets {};

    std::array<kernel::node_id, skeleton::node_count<skeleton_t>> evaluation_order {};

    std::array<kernel::node_id, skeleton::input_count<skeleton_t>> input_ids {};
    std::array<const char*, skeleton::input_count<skeleton_t>> input_names {};

    std::array<kernel::node_id, skeleton::output_count<skeleton_t>> output_ids {};
    std::array<const char*, skeleton::output_count<skeleton_t>> output_names {};

    // The user value types the network carries, deduplicated by discovery
    // order; a node emits at most one type, so the node budget bounds it.
    std::size_t type_count { 0 };
    std::array<const kernel::user_type*, skeleton::node_count<skeleton_t>> types {};

    std::array<kernel::crossing, skeleton::edge_count<skeleton_t>> crossings {};

    constexpr
    kernel::descriptor_view
    descriptor() const
    {
        return kernel::descriptor_view
        {
            std::span<const kernel::node_descriptor>{ nodes.data(), node_count },
            std::span<const kernel::node_label>{ labels.data(), node_count },
            std::span<const kernel::edge>{ edges.data(), edge_count },
            std::span<const kernel::node_id>{ fan_in.data(), edge_count },
            std::span<const std::size_t>{ fan_in_offsets.data(), node_count + 1 },
            std::span<const kernel::node_id>{ evaluation_order.data(), node_count },
            std::span<const kernel::region>{},
            std::span<const kernel::crossing>{ crossings.data(), crossing_count },
            std::span<const kernel::node_id>{ input_ids.data(), input_count },
            std::span<const kernel::node_id>{ output_ids.data(), output_count },
            std::span<const char* const>{ input_names.data(), input_count },
            std::span<const char* const>{ output_names.data(), output_count },
            std::span<const kernel::user_type* const>{ types.data(), type_count },
        };
    }
};

template<typename skeleton_t>
constexpr
std::optional<kernel::node_descriptor>
at(const descriptor_storage<skeleton_t>& network, kernel::node_id target)
{
    for (std::size_t i { 0 }; i < network.node_count; ++i)
        if (id(network.nodes[i]) == target)
            return network.nodes[i];

    return std::nullopt;
}

// A network that *names* a described user type is not a constant expression:
// `describe<T>::type()` computes field offsets at static-init time
// (`member_offset` is not a C++20 constant expression), so `compile` of such a
// network fails to evaluate at compile time even though it is a valid runtime
// call. Scalar networks stay `constexpr`; user-typed networks use the ordinary
// (runtime) recipe path. See kernel/user_type.hpp.
/// Lower a recipe into a constant descriptor at compile time. Scalar networks
/// stay `constexpr`; a network naming a described user type is a valid runtime
/// call but not a constant expression (field offsets are computed at
/// static-init time), so it uses the ordinary recipe path instead.
template<typename in_t, typename out_t, typename skeleton_t, std::size_t N>
constexpr
descriptor_storage<skeleton_t>
compile(const recipe<in_t, out_t, skeleton_t, N>& network)
{
    draft draft;
    std::optional<kernel::clock_tag> clock;

    instantiate(draft, clock, network);

    descriptor_storage<skeleton_t> result;

    // Nodes
    for (const auto& node : draft.nodes())
        result.nodes[result.node_count++] = node;

    // Labels (parallel to nodes; purely descriptive).
    {
        std::size_t label_index { 0 };
        for (const auto& label : draft.labels())
            result.labels[label_index++] = label;
    }

    // Edges
    for (const auto& edge : draft.edges())
        result.edges[result.edge_count++] = edge;

    // Fan-ins
    std::size_t fan_in_index { 0 };
    for (std::size_t node_index { 0 }; node_index < result.node_count; ++node_index)
    {
        result.fan_in_offsets[node_index] = fan_in_index;

        for (std::size_t edge_index { 0 }; edge_index < result.edge_count; ++edge_index)
            if (result.edges[edge_index].to == node_index)
                result.fan_in[fan_in_index++] = result.edges[edge_index].from;
    }
    result.fan_in_offsets[result.node_count] = fan_in_index;

    // Evaluation order
    const auto evaluation_order
    {
        kernel::evaluation_order(
            std::span<const kernel::node_descriptor>{ result.nodes.data(), result.node_count },
            std::span<const kernel::edge>{ result.edges.data(), result.edge_count }
        )
    };
    for (std::size_t i { 0 }; i < evaluation_order.size(); ++i)
        result.evaluation_order[i] = evaluation_order[i];

    // Inputs
    result.input_count = draft.context.input_ids.size();
    for (std::size_t i { 0 }; i < result.input_count; ++i)
    {
        result.input_ids[i] = draft.context.input_ids[i];
        result.input_names[i] = draft.context.input_names[i];
    }

    // Outputs
    result.output_count = draft.context.output_ids.size();
    for (std::size_t i { 0 }; i < result.output_count; ++i)
    {
        result.output_ids[i] = draft.context.output_ids[i];
        result.output_names[i] = draft.context.output_names[i];
    }

    // User value types (deduplicated by the walk; bounded by the node budget)
    for (const auto* type : draft.context.used_types)
        result.types[result.type_count++] = type;

    // Regions
    for (std::size_t node_i { 0 }; node_i < result.node_count; ++node_i)
    {
        /* TODO Fill regions, preserving parity with topology.hpp
        const auto root { find(parent, node_i) };

        if (region_by_root[root] == node_count)
        {
            region_by_root[root] = regions.size();
            regions.push_back({ clock(at(graph, root)), {} });
        }

        regions[region_by_root[root]].nodes.push_back(node_i);
        region_of[node_i] = region_by_root[root];
        */
    }

    // Sharing points
    for (const auto edge : result.edges)
    {
        const auto node_from { at(result, edge.from) };
        assert(node_from);

        const auto node_to { at(result, edge.to) };
        assert(node_to);

        if (kernel::clock(*node_from) != kernel::clock(*node_to))
        {
            const auto policy { draft.context.crossing_map.value_for_key(edge.from) };
            if (!policy)
                throw std::runtime_error{ "cross-clock edge without a crossing: annotate it with at<>" };

            result.crossings[result.crossing_count++] = { edge.from, edge.to, *policy };
        }
    }

    return result;
}

} // namespace clockr::shell
