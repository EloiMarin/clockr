#pragma once

/// @file
/// The lowering draft: the mutable scratch structure the recipe walk fills
/// with plain-data nodes, edges, crossings and port tables before a `program`
/// takes ownership. Internal to the shell/kernel boundary.

#include "clockr/conc/link.hpp"

#include "clockr/kernel/node_descriptor.hpp"
#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/node.hpp"
#include "clockr/kernel/node_label.hpp"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace clockr::shell
{

namespace detail
{

// The delay trampoline: a delay node's tick is only ever called to produce
// its initial value (the engine drives delay state thereafter, see
// engine.hpp evaluate_nodes), so the tick ignores its inputs and returns the
// value held in the cell. One address serves every delay node.
kernel::value
delay_tick(const void* self, const kernel::tick_context& ctx, std::span<const kernel::value> inputs);

} // namespace detail

// A constexpr-friendly associative container backed by two parallel vectors.
// Used by the walk's build context; it is transient and never escapes compile.
template<typename key_t, typename value_t>
class flat_map
{
public:
    constexpr
    value_t&
    operator [](key_t key)
    {
        const auto key_it { std::ranges::find(m_keys, key) };
        if (key_it == std::end(m_keys))
            throw std::runtime_error("Key not found");

        return m_values[static_cast<std::size_t>(key_it - std::begin(m_keys))];
    }

    constexpr
    bool
    contains(key_t key) const noexcept
    {
        return std::ranges::find(m_keys, key) != std::end(m_keys);
    }

    template<typename... Args>
    constexpr
    value_t&
    try_emplace(key_t key, Args... args)
    {
        const auto key_it { std::ranges::find(m_keys, key) };
        if (key_it != std::end(m_keys))
            return m_values[static_cast<std::size_t>(key_it - std::begin(m_keys))];

        m_keys.push_back(key);
        return m_values.emplace_back(std::forward<Args>(args)...);
    }

    constexpr
    std::size_t
    size() const noexcept
    {
        return m_keys.size();
    }

    template <typename key_lookup_t>
        requires std::equality_comparable_with<key_t, key_lookup_t>
    constexpr
    const value_t*
    value_for_key(key_lookup_t const& key_lookup) const noexcept
    {
        const auto key_it { std::ranges::find(m_keys, key_lookup) };
        if (key_it == std::end(m_keys))
            return nullptr;

        return &m_values[static_cast<std::size_t>(key_it - std::begin(m_keys))];
    }

private:
    std::vector<key_t> m_keys;
    std::vector<value_t> m_values;
};

class draft
{
public:
    // A cell record pairs the `self` pointer that lives in a compiled node with
    // a function that can clone the pointed-to closure into heap-owned storage.
    // The compile path ignores this (self points into the constexpr recipe);
    // the runtime build path uses it to own the cells in kernel::graph.
    struct closure_record
    {
        const void* self;
        std::shared_ptr<void>(*clone)(const void*);
    };

    constexpr
    kernel::node_id
    add_node(
        kernel::clock_tag clock,
        kernel::node_kind kind,
        kernel::node_descriptor::tick_fn tick,
        const void* self = nullptr,
        kernel::node_label label = {}
    )
    {
        const auto id { m_nodes.size() };

        m_nodes.emplace_back(id, clock, kind, tick, self);
        m_labels.emplace_back(label);
        m_fan_in.emplace_back();

        return id;
    }

    // A delay node always ticks through the same trampoline (it produces its
    // seed once; the engine drives the state afterwards). `materialize` is
    // non-null for a user-typed seed, which the program turns into a kernel
    // value at construction — see node_descriptor::materialize_fn.
    constexpr
    kernel::node_id
    add_delay(
        kernel::clock_tag clock,
        const void* self,
        kernel::node_descriptor::materialize_fn materialize = nullptr,
        kernel::node_label label = {}
    )
    {
        const auto delay_id
        {
            add_node(clock, kernel::node_kind::delay, &detail::delay_tick, self, label)
        };

        m_nodes[delay_id].materialize = materialize;
        context.delay_ids.push_back(delay_id);

        return delay_id;
    }

    constexpr
    void
    connect(kernel::node_id from, kernel::node_id to)
    {
        m_edges.emplace_back(kernel::edge{ from, to });
        m_fan_in[to].emplace_back(from);
    }

    constexpr
    void
    register_cell(const void* self, std::shared_ptr<void>(*clone)(const void*))
    {
        m_cells.push_back({ self, clone });
    }

    // Collect the user types a network *directly* names — one per node's own
    // value type — deduplicated, in discovery order. The pointers are to static
    // `user_type`s and live forever. Types reachable only as a nested field are
    // deliberately not collected here: `kernel::program` expands the registry
    // from the field schemas when it is built, which keeps this list within the
    // compiled descriptor's compile-time budget (a node names at most one
    // type).
    constexpr
    void
    register_type(const kernel::user_type* type)
    {
        // Anonymous types (id 0) have no schema to describe; listing one would
        // only add an empty entry every value already renders as `struct`.
        if (type == nullptr || type->id == 0)
            return;

        for (const auto* existing : context.used_types)
            if (existing == type)
                return;

        context.used_types.push_back(type);
    }

    // Emitted-node/edge totals, for the walk-vs-skeleton consistency assert in
    // shell::instantiate.
    constexpr
    std::size_t
    node_count() const noexcept
    {
        return m_nodes.size();
    }

    constexpr
    std::size_t
    edge_count() const noexcept
    {
        return m_edges.size();
    }

    // Read access for the boundary materializers (`shell::build` → `kernel::graph`,
    // `shell::compile` → `descriptor_storage`): `draft` is a passive accumulator and
    // owns no packing logic, so the `skeleton_t`-shaped conversion lives at the
    // `recipe<in_t, out_t, skeleton_t>` boundary, not here.
    constexpr auto nodes() const noexcept -> std::span<const kernel::node_descriptor>
    {
        return m_nodes;
    }

    // Per-node labels, parallel to `nodes()` (index == node id). Descriptive
    // only; the descriptor carries them so tools can name a node.
    constexpr auto labels() const noexcept -> std::span<const kernel::node_label>
    {
        return m_labels;
    }

    constexpr auto edges() const noexcept -> std::span<const kernel::edge>
    {
        return m_edges;
    }

    constexpr auto cells() const noexcept -> std::span<const closure_record>
    {
        return m_cells;
    }

    struct port
    {
        kernel::clock_tag region;
        kernel::node_id node_id;

        // Fire-and-forget inputs (optional-typed ports) revert to disengaged
        // after the tick that consumes them; plain level ports persist.
        bool event { false };
    };

    // Build-time state shared by every recipe during one instantiation. Thread
    // placement travels in the walker's `clock`, not here — except for
    // the share memoization, which keys on (share id, clock at entry): the
    // same share node used on two different clocks is instantiated twice, not
    // silently re-used (a shared fragment is "once per (share id, clock)").
    struct build_context
    {
        // Keyed by the stable by-value id minted in `share()` (ast.hpp's
        // share_t) — not by address — so a runtime arena may move/copy its
        // nodes without invalidating memoization ("ids, not addresses").
        // std::pair has no standard hash, so flat_map (linear) rather than an
        // unordered map.
        flat_map<std::pair<std::uint64_t, std::optional<kernel::clock_tag>>, std::vector<kernel::node_id>> shared_nodes;

        // For a share whose inner fragment is an input port, the port name
        // seen at first instantiation, keyed like `shared_nodes`. A share id
        // is nominal (one source location), so a helper parameterized by port
        // name would silently collapse its second port onto the first; the
        // walk compares the name here and rejects it (see share_t::instantiate).
        flat_map<std::pair<std::uint64_t, std::optional<kernel::clock_tag>>, std::string_view> shared_input_names;
        std::vector<kernel::node_id> input_ids;
        std::vector<kernel::node_id> output_ids;
        std::vector<kernel::node_id> delay_ids;
        std::vector<const char*> input_names;
        std::vector<const char*> output_names;
        flat_map<std::string_view, port> input_map;
        flat_map<std::string_view, port> output_map;
        flat_map<kernel::node_id, conc::policy> crossing_map;

        // User value types discovered during the walk (deduplicated), for the
        // program's debug schema registry.
        std::vector<const kernel::user_type*> used_types;
    };

    build_context context;

private:
    std::vector<kernel::node_descriptor> m_nodes;
    std::vector<kernel::node_label> m_labels;
    std::vector<kernel::edge> m_edges;
    std::vector<std::vector<kernel::node_id>> m_fan_in;
    std::vector<closure_record> m_cells;
};

} // namespace clockr::shell
