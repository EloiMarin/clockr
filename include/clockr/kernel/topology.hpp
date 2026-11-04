#pragma once

#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/node_descriptor.hpp"

#include "clockr/conc/link.hpp"
#include "clockr/shell/draft.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <numeric>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace clockr::kernel
{

namespace
{

constexpr
std::size_t
find(std::span<std::size_t> parent, std::size_t x);

constexpr
void
unite(std::span<std::size_t> parent, std::size_t a, std::size_t b);

} // namespace

/// A deterministic topological order of the graph (Kahn's algorithm,
/// lowest-ready-id first). Delay nodes are seeds: their tick-t value is
/// already known at tick start, so edges into a delay do not contribute
/// in-degree and a cycle through a delay is legal. A delay-free cycle throws
/// `std::runtime_error` (law L7).
template<node_c node_t>
constexpr
std::vector<node_id>
evaluation_order(std::span<const node_t> nodes, std::span<const edge> edges)
{   // Topological order of g using Kahn's algorithm.
    // Delay nodes are seeds: their tick-t value is already known at tick start
    // (it is tick t-1's result), so edges *into* a delay never add to the
    // in-degree. Feedback through a delay therefore never breaks the
    // topological order; cycles are rejected only when delay-free (L7).

    std::vector<bool> is_delay(nodes.size(), false);

    for (const auto& node : nodes)
        if (kind(node) == node_kind::delay)
            is_delay[id(node)] = true;

    std::vector<std::size_t> in_degree(nodes.size(), 0);

    for (const auto& edge : edges)
    {
        if (!is_delay[edge.to])
            ++in_degree[edge.to];
    }

    std::vector<node_id> ready;
    const auto push_ready
    {
        [&](node_id id)
        {
            ready.push_back(id);
            std::ranges::sort(
                ready,
                [](node_id a, node_id b) { return a > b; }
            );
        }
    };

    for (node_id i { 0 }; i < nodes.size(); ++i)
    {
        if (in_degree[i] == 0)
            push_ready(i);
    }

    std::vector<node_id> order;
    order.reserve(nodes.size());

    while (!ready.empty())
    {
        const auto node_id { ready.back() };
        ready.pop_back();

        order.push_back(node_id);

        for (const auto& edge : edges)
        {
            if (edge.from != node_id)
                continue;

            if (is_delay[edge.to])
                continue;

            if (--in_degree[edge.to] == 0)
                push_ready(edge.to);
        }
    }

    if (order.size() != nodes.size())
        throw std::runtime_error { "graph contains a cycle" };

    return order;
}

// The order is a fact about the graph's shape, so it must be computable
// during constant evaluation over plain data. Every TU including this header
// re-verifies the guarantee at zero runtime
// cost; the probe doubles as the L7 seed rule (the delay is a Kahn seed, so
// the guarded cycle passes constant evaluation).
static_assert(
    []
    {
        constexpr node_descriptor::tick_fn tick
        {
            [](const void*, const tick_context&, std::span<const value>) { return value {}; }
        };

        constexpr std::array<const node_descriptor, 3> nodes
        {
            node_descriptor { 0, clock_tag::ui, node_kind::input, tick, nullptr },
            node_descriptor { 1, clock_tag::ui, node_kind::delay, tick, nullptr },
            node_descriptor { 2, clock_tag::ui, node_kind::internal, tick, nullptr }
        };

        constexpr std::array<const edge, 3> edges
        {
            edge { 0, 1 }, edge { 1, 2 }, edge { 2, 1 }
        };

        return evaluation_order<const node_descriptor>(nodes, edges)
            == std::vector<node_id> { 0, 1, 2 };
    }(),
    "evaluation_order is a constant expression over plain-data nodes and edges"
);

/// One same-clock connected component: the nodes evaluated together by one
/// `engine::step` call.
struct region
{
    clock_tag clock;
    std::vector<node_id> nodes;
};

/// A cross-clock edge: a `from` value resampled into a `to` consumer under
/// `policy`.
struct crossing
{
    node_id from;
    node_id to;
    conc::policy policy;
};

/// The structural decomposition of a graph: its regions, a node-to-region
/// map, and the crossings between regions.
struct processed_regions
{
    std::vector<region> regions;
    std::vector<std::size_t> region_of;
    std::vector<crossing> crossings;
};

/// Split `graph` into same-clock regions and collect its crossings. Every
/// cross-clock edge must carry a policy in `crossing_map` (written by `at<>`);
/// an unannotated crossing throws.
template<graph_c graph_t>
constexpr
processed_regions
process_regions(const graph_t& graph, std::size_t node_count, const shell::flat_map<node_id, conc::policy>& crossing_map)
{
    std::vector<std::size_t> parent(node_count);
    std::iota(parent.begin(), parent.end(), 0);

    for (const auto edge : edges(graph))
        if (clock(at(graph, edge.from)) == clock(at(graph, edge.to)))
            unite(parent, edge.from, edge.to);

    std::vector<std::size_t> region_of;
    region_of.assign(node_count, 0);

    std::vector<std::size_t> region_by_root(node_count, node_count);

    std::vector<region> regions;
    for (std::size_t node_i { 0 }; node_i < node_count; ++node_i)
    {
        const auto root { find(parent, node_i) };

        if (region_by_root[root] == node_count)
        {
            region_by_root[root] = regions.size();
            regions.push_back({ clock(at(graph, root)), {} });
        }

        regions[region_by_root[root]].nodes.push_back(node_i);
        region_of[node_i] = region_by_root[root];
    }

    // Crossings: one per cross-clock edge.
    // A source fanning out to N consumers records N points.
    std::vector<crossing> crossings;
    for (const auto edge : edges(graph))
    {
        if (clock(at(graph, edge.from)) != clock(at(graph, edge.to)))
        {
            const auto policy { crossing_map.value_for_key(edge.from) };

            if (!policy)
                throw std::runtime_error{ "cross-clock edge without a crossing: annotate it with at<>" };

            crossings.push_back({ edge.from, edge.to, *policy });
        }
    }

    return { regions, region_of, crossings };
}

/// The immutable topology derived from a graph: evaluation order, regions,
/// crossings and the node-to-region map. Computing it is a construction-time
/// cost, never a tick-path cost.
template<graph_c graph_t>
struct topology
{
    topology() = default;
    explicit topology(const graph_t& graph, const shell::flat_map<node_id, conc::policy>& crossing_map)
    {
        evaluation_order = clockr::kernel::evaluation_order(nodes(graph), edges(graph));

        auto processed_regions { process_regions(graph, size(graph), crossing_map) };
        regions = processed_regions.regions;
        crossings = processed_regions.crossings;
        region_of = processed_regions.region_of;
    }

    std::vector<node_id> evaluation_order;
    std::vector<region> regions;
    std::vector<crossing> crossings;
    std::vector<std::size_t> region_of;
};

template<graph_c graph_t>
topology(graph_t, const shell::flat_map<node_id, conc::policy>&) -> topology<graph_t>;

namespace
{

constexpr
std::size_t
find(std::span<std::size_t> parent, std::size_t x)
{
    while (parent[x] != x)
    {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }

    return x;
}

constexpr
void
unite(std::span<std::size_t> parent, std::size_t a, std::size_t b)
{
    a = find(parent, a);
    b = find(parent, b);

    if (a != b)
        parent[b] = a;
}

} // namespace

} // namespace clockr::kernel
