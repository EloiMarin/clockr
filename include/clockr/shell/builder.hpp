#pragma once

#include "clockr/shell/recipe.hpp"

#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/node.hpp"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>

namespace clockr::shell
{

/// Lower a recipe into the structural `kernel::graph` (nodes, edges and
/// fan-in) used for topology analysis and tests. Execution goes through
/// `kernel::program`, not through this graph.
template<typename in_t, typename out_t, typename skeleton_t, std::size_t N>
kernel::graph
build(const recipe<in_t, out_t, skeleton_t, N>& network);

} // namespace clockr::shell

// ======= IMPLEMENTATION =======
namespace clockr::shell
{

template<typename in_t, typename out_t, typename skeleton_t, std::size_t N>
kernel::graph
build(const recipe<in_t, out_t, skeleton_t, N>& network)
{
    draft draft;
    std::optional<kernel::clock_tag> clock;

    instantiate(draft, clock, network);

    kernel::graph graph;

    for (const auto& node : draft.nodes())
    {
        if (node.materialize != nullptr)
        {
            // The structural graph form has no region pools: materialize a user-typed seed
            // on the heap and release it with the graph.
            const std::shared_ptr<kernel::value> seed
            {
                new kernel::value { node.materialize(node.self, nullptr) },
                [](kernel::value* value)
                {
                    kernel::release(*value);
                    delete value;
                }
            };

            graph.add_node(
                node.clock,
                node.kind,
                [seed, tick_fn = node.tick](const kernel::tick_context& ctx, std::span<const kernel::value> inputs)
                {
                    return tick_fn(seed.get(), ctx, inputs);
                }
            );

            continue;
        }

        if (node.self)
        {
            const auto cell_it
            {
                std::find_if(
                    std::begin(draft.cells()),
                    std::end(draft.cells()),
                    [self_ptr = node.self](const auto& record)
                    {
                        return self_ptr == record.self;
                    }
                )
            };
            assert(cell_it != std::end(draft.cells()));

            const std::shared_ptr<void> cell { cell_it->clone(node.self) };

            graph.add_node(
                node.clock,
                node.kind,
                [cell, tick_fn = node.tick](const kernel::tick_context& ctx, std::span<const kernel::value> inputs)
                {
                    return tick_fn(cell.get(), ctx, inputs);
                }
            );
        }
        else
        {
            graph.add_node(
                node.clock,
                node.kind,
                [tick_fn = node.tick](const kernel::tick_context& ctx, std::span<const kernel::value> inputs)
                {
                    return tick_fn(nullptr, ctx, inputs);
                }
            );
        }
    }

    for (const auto& edge : draft.edges())
        graph.connect(edge.from, edge.to);

    return graph;
}

} // namespace clockr::shell
