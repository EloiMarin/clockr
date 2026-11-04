#include "clockr/kernel/program.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>

namespace clockr::kernel
{

program::program(descriptor_view desc, program_options options) :
    m_options { options }
{
    // Plain data is copied in; closure `self` pointers (and port-name bytes)
    // keep borrowing the caller's storage: static descriptors and literals
    // outlive use by construction, which is the contract here.
    m_nodes.assign(desc.nodes.begin(), desc.nodes.end());
    m_labels.assign(desc.labels.begin(), desc.labels.end());
    m_edges.assign(desc.edges.begin(), desc.edges.end());
    m_fan_in.assign(desc.fan_in.begin(), desc.fan_in.end());
    m_fan_in_offsets.assign(desc.fan_in_offsets.begin(), desc.fan_in_offsets.end());
    m_input_ids.assign(desc.input_ids.begin(), desc.input_ids.end());
    m_input_names.assign(desc.input_names.begin(), desc.input_names.end());
    m_output_ids.assign(desc.output_ids.begin(), desc.output_ids.end());
    m_output_names.assign(desc.output_names.begin(), desc.output_names.end());
    m_types.assign(desc.types.begin(), desc.types.end());

    // Name directory from the descriptor's parallel spans (region from the
    // node thread — same rule construct_region_runtime partitions by, so the
    // two can never disagree). Duplicate names are a build-time error, as in
    // the draft path.
    assert(m_input_names.size() == m_input_ids.size());
    assert(m_output_names.size() == m_output_ids.size());

    for (std::size_t i { 0 }; i < m_input_ids.size(); ++i)
    {
        if (m_input_map.contains(m_input_names[i]))
            throw std::runtime_error
            {
                "Input port name already exists: " + std::string{ m_input_names[i] }
                + " (to fan one port out to several consumers, bind it once with shell::share(...))"
            };

        m_input_map.try_emplace(
            m_input_names[i],
            shell::draft::port{ clock(at(descriptor(), m_input_ids[i])), m_input_ids[i] });
    }

    for (std::size_t i { 0 }; i < m_output_ids.size(); ++i)
    {
        if (m_output_map.contains(m_output_names[i]))
            throw std::runtime_error{ "Output port name already exists: " + std::string{ m_output_names[i] } };

        m_output_map.try_emplace(
            m_output_names[i],
            shell::draft::port{ clock(at(descriptor(), m_output_ids[i])), m_output_ids[i] });
    }

    // The stored evaluation order must be the topological one (shell::compile
    // derives it from the same algorithm); hand-written descriptors get the
    // same pin for free.
    shell::flat_map<node_id, conc::policy> crossings;

    for (const auto& crossing : desc.crossings)
        crossings.try_emplace(crossing.from, crossing.policy);

    init(crossings);

    assert(
        std::ranges::equal(
            std::span<const node_id>{ desc.evaluation_order },
            m_topology.evaluation_order));

    materialize_seeds();
}

program::~program()
{
    // Program destruction is a quiescent point: no region is ticking, so every
    // outstanding user-value reference can be dropped here. Order does not
    // matter for correctness — the homes (and their pools) are still alive
    // (destructor bodies run before member destruction) — but it does make a
    // destruction-time leak visible: every cell a program owns must reach zero
    // here, and any remaining reference is a lifecycle bug elsewhere.
    for (std::size_t i { 0 }; i < clock_tag_count; ++i)
    {
        const auto tag { static_cast<clock_tag>(i) };
        const auto* runtime { m_region_runtime.value_for_key(tag) };

        if (runtime == nullptr)
            continue;

        for (const auto& input : runtime->inputs)
            release(input);

        for (const auto& output : runtime->outputs)
            release(output);

        for (const auto& state : runtime->delay_states)
            if (state.has_value())
                release(*state);

        // Latest rings: every slot holds a reference; hand them all back.
        // `sends_latest` lists each latest source once.
        for (const auto source : runtime->sends_latest)
        {
            m_links_latest[source].release_all(
                [](const value& held) noexcept { release(held); });
        }

        // Queued backlogs: every link entry holds a reference; drain and
        // release. `sends_queued` lists each queued source once.
        constexpr auto newest { std::numeric_limits<conc::epoch_id>::max() };

        for (const auto source : runtime->sends_queued)
        {
            value received;

            while (m_links_queued[source].receive(received, newest))
                release(received);
        }
    }

    // Materialized seeds hold one reference each, from their region's pool.
    for (const auto& seed : m_seed_values)
        release(seed);

    // Destruction is quiescent: no reader can be in flight, so every retired
    // cell can be destroyed and freed outright, without the grace period.
    for (const auto& home : m_homes)
    {
        if (home == nullptr)
            continue;

        while (user_cell* cell { home->retired.pop() })
            ::clockr::kernel::reclaim(cell, &home->pool);

        while (user_cell* cell { home->deferred.pop() })
            ::clockr::kernel::reclaim(cell, &home->pool);
    }

#ifndef NDEBUG
    // Every cell the program created flows back to its home pool; the pools
    // must therefore be empty now. A non-empty pool is a lifecycle bug (a
    // forgotten release or a double-retain) surfaced at destruction instead of
    // as a silent leak.
    for (const auto& home : m_homes)
        assert((home == nullptr || home->pool.snapshot().used_bytes == 0) && "program destroyed with live user cells");
#endif
}

descriptor_view
program::descriptor() const noexcept
{
    return descriptor_view
    {
        m_nodes,
        m_labels,
        m_edges,
        m_fan_in,
        m_fan_in_offsets,
        m_topology.evaluation_order,
        std::span<const struct region>{},
        std::span<const struct crossing>{},
        m_input_ids,
        m_output_ids,
        m_input_names,
        m_output_names,
        std::span<const user_type* const>{ m_types.data(), m_types.size() },
    };
}

struct clockr::kernel::topology<descriptor_view>&
program::topology() noexcept
{
    return m_topology;
}

const struct clockr::kernel::topology<descriptor_view>&
program::topology() const noexcept
{
    return m_topology;
}

void
program::set(std::string_view input_port_name, value value)
{
    const auto maybe_port { m_input_map.value_for_key(input_port_name) };

    if (!maybe_port)
        throw std::runtime_error{ "Input port not found: " + std::string{ input_port_name } };

    auto& runtime { m_region_runtime[maybe_port->region] };

    assert(!runtime.in_step.load(std::memory_order_relaxed)
           && "input port set while its region was ticking: set must be quiescent with the region tick");

    const auto slot { input_slot(runtime, maybe_port->node_id) };

    retain_assign(runtime.inputs[slot], value);
    runtime.input_dirty[slot] = std::byte{ 1 };
}

void
program::set(std::size_t index, value v)
{
    if (index >= m_input_ids.size())
        throw std::runtime_error{ "Input index out of range: " + std::to_string(index) };

    const auto id { m_input_ids[index] };
    auto& runtime { m_region_runtime[clock(at(descriptor(), id))] };

    assert(!runtime.in_step.load(std::memory_order_relaxed)
           && "input port set while its region was ticking: set must be quiescent with the region tick");

    const auto slot { input_slot(runtime, id) };

    retain_assign(runtime.inputs[slot], v);
    runtime.input_dirty[slot] = std::byte{ 1 };
}

value
program::get(std::size_t index) const
{
    if (index >= m_output_ids.size())
        throw std::runtime_error{ "Output index out of range: " + std::to_string(index) };

    const auto id { m_output_ids[index] };
    const auto* runtime { m_region_runtime.value_for_key(clock(at(descriptor(), id))) };

    assert(runtime && "region runtimes are built for every thread at construction");
    assert(!runtime->in_step.load(std::memory_order_relaxed)
           && "output port read while its region was ticking: get must be quiescent with the region tick");

    return runtime->outputs[output_slot(*runtime, id)];
}

value
program::get_input(std::size_t index) const
{
    if (index >= m_input_ids.size())
        throw std::runtime_error{ "Input index out of range: " + std::to_string(index) };

    const auto id { m_input_ids[index] };
    const auto* runtime { m_region_runtime.value_for_key(clock(at(descriptor(), id))) };

    assert(runtime && "region runtimes are built for every thread at construction");
    assert(!runtime->in_step.load(std::memory_order_relaxed)
           && "input port read while its region was ticking: get_input must be quiescent with the region tick");

    return runtime->inputs[input_slot(*runtime, id)];
}

value
program::get(std::string_view output_port_name) const
{
    const auto maybe_port { m_output_map.value_for_key(output_port_name) };

    if (!maybe_port)
        throw std::runtime_error{ "Output port not found: " + std::string{ output_port_name } };

    const auto* runtime { m_region_runtime.value_for_key(maybe_port->region) };

    assert(runtime && "region runtimes are built for every thread at construction");
    assert(!runtime->in_step.load(std::memory_order_relaxed)
           && "output port read while its region was ticking: get must be quiescent with the region tick");

    return runtime->outputs[output_slot(*runtime, maybe_port->node_id)];
}

value
program::get_input(std::string_view input_port_name) const
{
    const auto maybe_port { m_input_map.value_for_key(input_port_name) };

    if (!maybe_port)
        throw std::runtime_error{ "Input port not found: " + std::string{ input_port_name } };

    const auto* runtime { m_region_runtime.value_for_key(maybe_port->region) };

    assert(runtime && "region runtimes are built for every thread at construction");
    assert(!runtime->in_step.load(std::memory_order_relaxed)
           && "input port read while its region was ticking: get_input must be quiescent with the region tick");

    return runtime->inputs[input_slot(*runtime, maybe_port->node_id)];
}

auto
program::region(clock_tag tag) -> region_runtime&
{
    assert(m_region_runtime.contains(tag) && "region runtimes are built for every thread at construction");

    return m_region_runtime[tag];
}

auto
program::region(clock_tag tag) const -> const region_runtime&
{
    assert(m_region_runtime.contains(tag) && "region runtimes are built for every thread at construction");

    return *m_region_runtime.value_for_key(tag);
}

void
program::send_queued(node_id id, const value& v, conc::epoch_id epoch)
{
    if (!m_links_queued.contains(id))
        throw std::runtime_error{ "No queued link for node: " + std::to_string(id) };

    // The channel takes a reference. On a full queue the value is dropped
    // (L6 capacity precondition) — release the reference and count it.
    retain(v);

    if (!m_links_queued[id].send(v, epoch))
    {
        release(v);
        m_dropped_sends.fetch_add(1, std::memory_order_relaxed);
    }
}

void
program::send_latest(node_id id, const value& v, conc::epoch_id epoch)
{
    if (!m_links_latest.contains(id))
        throw std::runtime_error{ "No latest link for node: " + std::to_string(id) };

    // The target ring slot takes a reference, and hands back the reference the
    // entry it displaces held (after the store — see the link). Each slot
    // owns its own reference, so a value stays alive for as long as any slot,
    // or any reader, can still see it.
    retain(v);
    m_links_latest[id].send(
        v,
        epoch,
        [](const value& displaced) noexcept { release(displaced); });
}

std::optional<value>
program::receive(node_id id, conc::epoch_id published)
{
    if (m_links_queued.contains(id))
    {
        // Ownership transfer: the queue's reference moves to the caller.
        value v;
        if (!m_links_queued[id].receive(v, published))
            return std::nullopt;
        return v;
    }

    if (m_links_latest.contains(id))
    {
        // The reference is taken inside the slot's version window (see
        // conc::slot::load_retained): a producer displacing the value cannot
        // free it under this read, because reclamation honors the grace
        // period. The caller owns the returned reference.
        return m_links_latest[id].receive(
            published,
            [](const value& v) noexcept { return try_retain(v); },
            [](const value& v) noexcept { release(v); });
    }

    throw std::runtime_error{ "No link for node: " + std::to_string(id) };
}

std::size_t
program::input_slot(const region_runtime& runtime, node_id id)
{
    const auto it { std::ranges::find(runtime.input_ids, id) };

    if (it == std::end(runtime.input_ids))
        throw std::logic_error{ "Input ID not listed in its region: " + std::to_string(id) };

    return static_cast<std::size_t>(it - std::begin(runtime.input_ids));
}

std::size_t
program::output_slot(const region_runtime& runtime, node_id id)
{
    const auto it { std::ranges::find(runtime.output_ids, id) };

    if (it == std::end(runtime.output_ids))
        throw std::logic_error{ "Output ID not listed in its region: " + std::to_string(id) };

    return static_cast<std::size_t>(it - std::begin(runtime.output_ids));
}

void
program::init(const shell::flat_map<node_id, conc::policy>& crossings)
{
    // The registry starts as the types the network *directly* names (a node's
    // own value type, collected by the walk); expand it with the types
    // reachable only as nested fields, so the debugger has a schema for every
    // struct it can be asked to render. The vector has no static bound, so
    // nesting depth costs nothing here — the compiled descriptor stays bounded
    // by the node count because it carries only the direct set.
    for (std::size_t i { 0 }; i < m_types.size(); ++i)
    {
        for (const auto& field : m_types[i]->fields)
        {
            if (field.kind != field_kind::nested || field.nested == nullptr || field.nested->id == 0)
                continue;

            if (std::ranges::find(m_types, field.nested) == std::end(m_types))
                m_types.push_back(field.nested);
        }
    }

    // The registry must not contain two distinct types under one id: the debug
    // wire identifies a struct by its id, so a collision would render the wrong
    // schema. Anonymous types (id 0) are exempt — many share it by design.
    for (std::size_t i { 0 }; i < m_types.size(); ++i)
    {
        for (std::size_t j { i + 1 }; j < m_types.size(); ++j)
        {
            if (m_types[i] != m_types[j] && m_types[i]->id != 0 && m_types[i]->id == m_types[j]->id)
                throw std::runtime_error
                {
                    "Duplicate user type id: \""
                    + std::string { m_types[i]->name != nullptr ? m_types[i]->name : "?" }
                    + "\" and \""
                    + std::string { m_types[j]->name != nullptr ? m_types[j]->name : "?" }
                    + "\""
                };
        }
    }

    m_topology = clockr::kernel::topology<descriptor_view>{ descriptor(), crossings };

    // Every mapped port must agree with its node: the draft anchors a port's
    // region where its data enters, and the emitted node lives there too.
    // (The span path keeps no maps — its regions derive from node threads in
    // construct_region_runtime — so names that resolve to nothing are simply
    // skipped. Name lists parallel id lists on both paths.)
    assert(m_input_names.size() == m_input_ids.size());
    assert(m_output_names.size() == m_output_ids.size());

#ifndef NDEBUG
    for (std::size_t i { 0 }; i < m_input_ids.size(); ++i)
    {
        if (const auto* port { m_input_map.value_for_key(m_input_names[i]) })
        {
            assert(port->node_id == m_input_ids[i]);
            assert(clock(at(descriptor(), port->node_id)) == port->region);
        }
    }

    for (std::size_t i { 0 }; i < m_output_ids.size(); ++i)
    {
        if (const auto* port { m_output_map.value_for_key(m_output_names[i]) })
        {
            assert(port->node_id == m_output_ids[i]);
            assert(clock(at(descriptor(), port->node_id)) == port->region);
        }
    }
#endif

    for (std::size_t i { 0 }; i < clock_tag_count; ++i)
        construct_region_runtime(static_cast<clock_tag>(i));
}

bool
program::reclaimable(const user_cell* cell) const noexcept
{
    for (const auto& announce : m_grace.announce)
    {
        const auto epoch { announce.load(std::memory_order_acquire) };

        // A region mid-tick announced an epoch at or before the retirement:
        // it may still be inside a read that predates it.
        if (epoch != 0 && epoch <= cell->retire_epoch)
            return false;
    }

    return true;
}

std::size_t
program::reclaim(clock_tag tag)
{
    return reclaim(tag, m_options.reclaim_batch);
}

std::size_t
program::reclaim(clock_tag tag, std::size_t max_cells)
{
    auto* home { m_homes[clock_tag_index(tag)].get() };

    if (home == nullptr)
        return 0;

    std::size_t reclaimed { 0 };

    for (std::size_t i { 0 }; i < max_cells; ++i)
    {
        user_cell* cell { home->retired.pop() };

        if (cell == nullptr)
            break;

        if (reclaimable(cell))
        {
            // Qualified past program::descriptor(), which hides the namespace name
            // in member scope.
            ::clockr::kernel::reclaim(cell, &home->pool);
            ++reclaimed;
        }
        else
        {
            home->deferred.push(cell);
        }
    }

    // Rotate this pass's withheld cells back for the next pass. Bounded by the
    // pass budget, so a tick's worth of reclamation stays bounded.
    while (user_cell* cell { home->deferred.pop() })
        home->retired.push(cell);

    return reclaimed;
}

void
program::materialize_seeds()
{
    const auto seed_count
    {
        std::count_if(
            m_nodes.begin(),
            m_nodes.end(),
            [](const node_descriptor& node) { return node.materialize != nullptr; })
    };

    m_seed_values.clear();
    m_seed_values.reserve(static_cast<std::size_t>(seed_count));

    for (const auto& node : m_nodes)
    {
        if (node.materialize == nullptr)
            continue;

        const auto* home { m_region_runtime[clock_of(node.id)].home };
        m_seed_values.push_back(node.materialize(node.self, home));
    }

    // Repoint only after every value exists: the vector may have reallocated
    // while filling, and a `self` set too early would dangle.
    std::size_t seed_index { 0 };

    for (auto& node : m_nodes)
    {
        if (node.materialize != nullptr)
            node.self = &m_seed_values[seed_index++];
    }
}

void
program::construct_region_runtime(clock_tag tag)
{
    // The value home is program-owned and address-stable (cells point at it);
    // it owns the region's pool.
    const auto home_index { clock_tag_index(tag) };

    if (m_homes[home_index] == nullptr)
    {
        m_homes[home_index] = std::make_unique<value_home>(m_options.region_pool_bytes);
        m_homes[home_index]->grace = &m_grace;
        m_homes[home_index]->announce = &m_grace.announce[home_index];
    }

    m_region_runtime.try_emplace(tag);
    auto& runtime { m_region_runtime[tag] };
    runtime.home = m_homes[home_index].get();

    auto node_clock_of = [&](node_id id) { return clock(at(descriptor(), id)); };

    // Evaluation order: this region's nodes, in global Kahn order, so every
    // region agrees with the single deterministic linearization (L1).
    for (const auto id : m_topology.evaluation_order)
        if (clock_of(id) == tag)
            runtime.evaluation_order.push_back(id);

    // Port storage, partitioned by owning region (node thread; validated
    // against the draft's port region above on the recipe path).
    // Declaration/span order is preserved by filtering the global lists.
    // Event-ness travels with the draft port (span path: always level); the
    // dirty flags start clean — nothing has been set since any tick yet.
    for (std::size_t i = 0; i < m_input_ids.size(); ++i)
    {
        const auto id { m_input_ids[i] };

        if (node_clock_of(id) != tag)
            continue;

        runtime.input_ids.push_back(id);
        runtime.inputs.emplace_back();

        const auto* port { m_input_map.value_for_key(m_input_names[i]) };
        const std::byte is_event
        {
            (port != nullptr && port->event)
                ? std::byte{ 1 }
                : std::byte{ 0 }
        };

        runtime.input_is_event.push_back(is_event);
        runtime.input_dirty.push_back(std::byte{ 0 });
    }

    for (const auto id : m_output_ids)
        if (node_clock_of(id) == tag)
        {
            runtime.output_ids.push_back(id);
            runtime.outputs.emplace_back();
        }

    // Delay storage belongs to its node's region.
    for (const auto id : runtime.evaluation_order)
        if (kind(at(descriptor(), id)) == node_kind::delay)
        {
            runtime.delay_ids.push_back(id);
            runtime.delay_states.emplace_back();
        }

    // Id -> slot tables (RT-hardened lookup): built once here, read O(1) per
    // visited node. Duplicates and kind mismatches are construction bugs —
    // loud here, never silent misroutes on the tick path.
    const std::size_t node_count { m_topology.evaluation_order.size() };

    runtime.input_slot_of.assign(node_count, region_runtime::no_slot);
    runtime.output_slot_of.assign(node_count, region_runtime::no_slot);
    runtime.delay_slot_of.assign(node_count, region_runtime::no_slot);

#ifdef NDEBUG
    auto index = [&](const std::vector<node_id>& ids, std::vector<std::size_t>& table, node_kind /*expected*/)
#else
    auto index = [&](const std::vector<node_id>& ids, std::vector<std::size_t>& table, node_kind expected)
#endif
    {
        for (std::size_t slot { 0 }; slot < ids.size(); ++slot)
        {
            assert(ids[slot] < table.size()); // node ids are dense from construction
            assert(kind(at(descriptor(), ids[slot])) == expected);
            assert(table[ids[slot]] == region_runtime::no_slot);

            table[ids[slot]] = slot;
        }
    };

    index(runtime.input_ids, runtime.input_slot_of, node_kind::input);
    index(runtime.output_ids, runtime.output_slot_of, node_kind::output);
    index(runtime.delay_ids, runtime.delay_slot_of, node_kind::delay);

    // Coverage: every port/delay node this region will ever visit must
    // resolve. The engine trusts the tables unconditionally.
    for (const auto id : runtime.evaluation_order)
    {
        assert(id < node_count);

        switch (kind(at(descriptor(), id)))
        {
            case node_kind::input:
                if (runtime.input_slot_of[id] == region_runtime::no_slot)
                    throw std::logic_error{ "Input node not listed in its region: " + std::to_string(id) };
                break;
            case node_kind::output:
                if (runtime.output_slot_of[id] == region_runtime::no_slot)
                    throw std::logic_error{ "Output node not listed in its region: " + std::to_string(id) };
                break;
            case node_kind::delay:
                if (runtime.delay_slot_of[id] == region_runtime::no_slot)
                    throw std::logic_error{ "Delay node not listed in its region: " + std::to_string(id) };
                break;
            case node_kind::internal:
                break;
            default:
                break;
        }
    }

    // Link wiring, keyed by SOURCE node id: one pop/push per source per
    // tick, however many consumers read it. Sharing points are per crossing
    // edge, so every consumer is visited; push_unique collapses fan-out onto
    // the single shared link object (program-owned: producer and consumer
    // share one — regions hold only node ids).
    auto push_unique = [](std::vector<node_id>& list, node_id id)
    {
        if (std::ranges::find(list, id) == std::end(list))
            list.push_back(id);
    };

    for (const auto& crossing : m_topology.crossings)
    {
        const bool queued { crossing.policy == conc::policy::queued };

        if (clock_of(crossing.to) == tag)
            push_unique(
                queued ? runtime.receives_queued : runtime.receives_latest,
                crossing.from);

        if (clock_of(crossing.from) == tag)
            push_unique(
                queued ? runtime.sends_queued : runtime.sends_latest,
                crossing.from);

        if (queued)
            m_links_queued.try_emplace(crossing.from);
        else
            m_links_latest.try_emplace(crossing.from);
    }
}

clock_tag
program::clock_of(node_id id) const
{
    return m_topology.regions[m_topology.region_of[id]].clock;
}

} // namespace clockr::kernel
