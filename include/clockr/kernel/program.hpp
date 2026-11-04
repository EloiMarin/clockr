#pragma once

#include "clockr/kernel/descriptor.hpp"
#include "clockr/kernel/node_descriptor.hpp"
#include "clockr/kernel/node.hpp"
#include "clockr/kernel/topology.hpp"

#include "clockr/conc/link.hpp"
#include "clockr/mem/arena.hpp"
#include "clockr/mem/pool.hpp"
#include "clockr/mem/pool_allocator.hpp"
#include "clockr/shell/builder.hpp"
#include "clockr/shell/draft.hpp"
#include "clockr/shell/recipe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace clockr::kernel
{

/// One delay node's settled state: the value it will emit on the next tick.
using delay_state = std::optional<value>;

// program — the single execution plane of the library.
//
// A program owns everything the engine ticks except user closures: plain-data
// nodes/edges/fan-in tables, port directories, per-region value storage, and
// the links between regions. Two ways in, one runtime shape out:
//
//   1. From an recipe recipe (primary path): the recipe is walked once at
//      construction; stateful closures are CLONED into program-owned cells
//      (same mechanism as shell::build), so the recipe may die afterwards.
//      Only port-name bytes are borrowed (literals in practice).
//   2. From a descriptor span view (static descriptors from shell::compile,
//      hand-written literals in tests): plain data is copied in; closure
//      `self` pointers keep borrowing the recipe/literal storage, which must
//      therefore outlive the program.
//
// (The old runtime graph of std::function nodes is gone: dynamic graph
// mutation returns as a ROADMAP phase-F concern, at which point a runtime
// plane may be reintroduced alongside this one.)
/// Construction-time knobs. The pool byte count bounds every user value a
/// region can hold at once — live values in a tick, delay states, link
/// backlogs, and (in debug builds) recorder-retained values. Exhaustion
/// throws `std::bad_alloc` on the ticking thread, so size it with headroom;
/// the default fits ordinary demo traffic, not a large preset.
struct program_options
{
    /// Every region's pool size. See "Sizing the memory pools" in the root
    /// README: this is the one budget the host must compute for its own
    /// workload, and exhaustion throws `std::bad_alloc` on a ticking thread.
    std::size_t region_pool_bytes { 64 * 1024 };

    /// Upper bound on retired user cells one region reclaims per tick — the
    /// per-tick reclamation budget, so the ticking thread's worst case stays
    /// bounded. The rest waits for the next tick (or for a host's explicit
    /// `program::reclaim`).
    std::size_t reclaim_batch { 64 };
};

/// The single execution plane: owns the lowered descriptor, port directory,
/// per-region runtimes and the links between regions. Build it from a shell
/// recipe (owning; closures are cloned) or from a `descriptor_view`
/// (borrowing; the descriptor must outlive it), then tick its regions with an
/// `engine`.
class program
{
public:
    using options = program_options;

    /// One region's pre-built execution storage. `step(clock)` consumes only
    // this: the engine prepares no views over `program` on the tick path.
    //
    // Values sit next to the ids they belong to: `inputs[i]` is the
    // host-driven value for `input_ids[i]`, `outputs[i]` for `output_ids[i]`,
    // `delay_states[i]` for `delay_ids[i]`. Id and value vectors grow together
    // at construction and are never reordered afterwards, so a slot index
    // stays valid for the program's life. There are deliberately no global
    // input/output/state spans: the engine consumes regions, and the shell
    // addresses ports by name (recipe path) or declaration index (see set/get).
    //
    // The `*_slot_of` tables are the RT-hardened id -> slot index for the
    // tick path: built once at construction (sized to the graph node count,
    // `no_slot` where the node holds no such value), read O(1) per visited
    // node. Correctness never depends on visit order coinciding with fill
    // order — the engine resolves every visited node id through these, so a
    // reordered evaluation (or a future partial/gated tick) cannot misroute
    // values onto the wrong ports.
    struct region_runtime
    {
        static constexpr std::size_t no_slot = std::numeric_limits<std::size_t>::max();

        std::vector<node_id> evaluation_order; // this region's nodes, in global Kahn order
        std::vector<node_id> input_ids;
        std::vector<value> inputs;
        std::vector<std::size_t> input_slot_of; // indexed by node id
        std::vector<std::byte> input_is_event;
        std::vector<std::byte> input_dirty;
        std::vector<node_id> receives_queued;  // SOURCE ids fed by queued links
        std::vector<node_id> receives_latest;  // SOURCE ids fed by latest links
        std::vector<node_id> delay_ids;
        std::vector<delay_state> delay_states;
        std::vector<std::size_t> delay_slot_of; // indexed by node id
        std::vector<node_id> sends_queued;     // SOURCE ids feeding queued links
        std::vector<node_id> sends_latest;     // SOURCE ids feeding latest links
        std::vector<node_id> output_ids;
        std::vector<value> outputs;
        std::vector<std::size_t> output_slot_of; // indexed by node id

        // The region's value home — its pool and (later) reclamation state.
        // Program-owned and address-stable: user cells hold a raw pointer to
        // it, so it must not live in this movable struct. Set by
        // construct_region_runtime. Non-const because the tick path allocates
        // from its pool; cells hold it as `const value_home*`.
        value_home* home { nullptr };

        // Completed producer ticks (versioned-snapshot publication, §3.2).
        // Bumped once at the end of this region's tick (release); consumers
        // snapshot it once per tick per producer (acquire) so all channels
        // from one producer share a single cut.
        std::atomic<conc::epoch_id> published_epoch { 0 };

        // Debug-only overlap guard: held true while this region's tick is in
        // flight, so two threads ticking the same clock at once are caught
        // (a host bug). A region may move between threads between ticks
        // (thread pools); only overlap is forbidden. Touched only under
        // `#ifndef NDEBUG` (see engine's region_step_guard).
        std::atomic<bool> in_step { false };

        region_runtime() = default;

        region_runtime(const region_runtime&) = delete;
        region_runtime& operator=(const region_runtime&) = delete;

        region_runtime(region_runtime&& other) noexcept :
            evaluation_order { std::move(other.evaluation_order) },
            input_ids { std::move(other.input_ids) },
            inputs { std::move(other.inputs) },
            input_slot_of { std::move(other.input_slot_of) },
            input_is_event { std::move(other.input_is_event) },
            input_dirty { std::move(other.input_dirty) },
            receives_queued { std::move(other.receives_queued) },
            receives_latest { std::move(other.receives_latest) },
            delay_ids { std::move(other.delay_ids) },
            delay_states { std::move(other.delay_states) },
            delay_slot_of { std::move(other.delay_slot_of) },
            sends_queued { std::move(other.sends_queued) },
            sends_latest { std::move(other.sends_latest) },
            output_ids { std::move(other.output_ids) },
            outputs { std::move(other.outputs) },
            output_slot_of { std::move(other.output_slot_of) },
            home { other.home },
            published_epoch { other.published_epoch.load(std::memory_order_relaxed) },
            in_step { other.in_step.load(std::memory_order_relaxed) }
        {}

        region_runtime&
        operator=(region_runtime&& other) noexcept
        {
            if (this == &other)
                return *this;

            evaluation_order = std::move(other.evaluation_order);
            input_ids = std::move(other.input_ids);
            inputs = std::move(other.inputs);
            input_slot_of = std::move(other.input_slot_of);
            input_is_event = std::move(other.input_is_event);
            input_dirty = std::move(other.input_dirty);
            receives_queued = std::move(other.receives_queued);
            receives_latest = std::move(other.receives_latest);
            delay_ids = std::move(other.delay_ids);
            delay_states = std::move(other.delay_states);
            delay_slot_of = std::move(other.delay_slot_of);
            sends_queued = std::move(other.sends_queued);
            sends_latest = std::move(other.sends_latest);
            output_ids = std::move(other.output_ids);
            outputs = std::move(other.outputs);
            output_slot_of = std::move(other.output_slot_of);
            home = other.home;
            published_epoch.store(other.published_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
            in_step.store(other.in_step.load(std::memory_order_relaxed), std::memory_order_relaxed);

            return *this;
        }
    };

    /// Owning construction: lower `network`, cloning its stateful closures into
    /// program-owned cells. The recipe may die once construction finishes.
    template<typename in_t, typename out_t, typename skeleton_t, std::size_t N>
    explicit
    program(const shell::recipe<in_t, out_t, skeleton_t, N>& network, program_options options = {});

    /// Borrowing construction: copy the descriptor's plain data; closure
    /// `self` pointers and port-name bytes keep pointing at the caller's
    /// storage, which must outlive the program.
    explicit
    program(descriptor_view desc, program_options options = {});

    ~program();

    program(const program&) = delete;
    program& operator=(const program&) = delete;

    /// Transient view over the owned descriptor storage, for the engine and
    /// structural inspection. Its spans borrow this object: use it within the
    /// tick, never retain it.
    descriptor_view
    descriptor() const noexcept;

    /// The derived topology (regions, crossings, evaluation order).
    struct clockr::kernel::topology<descriptor_view>&
    topology() noexcept;

    /// The derived topology (regions, crossings, evaluation order).
    const struct clockr::kernel::topology<descriptor_view>&
    topology() const noexcept;

    /// Set an input port by name. Names resolve through the build directory to
    /// one region slot, so the tick path never pays for a lookup. Unknown
    /// names throw. Threading: call it quiescent-with the owning region's tick
    /// (same-thread set-then-tick, or an external happens-before).
    void
    set(std::string_view input_port_name, value);

    /// Construct a user struct into the port's region pool and set it. The
    /// host never holds a raw `value` for a struct: this is the owning path,
    /// and same-thread set-then-tick is what makes the pool allocation safe.
    template<typename T, typename... Args>
    requires std::constructible_from<T, Args...>
    void
    set(std::string_view input_port_name, Args&&... args);

    /// Read an output by name. Same quiescence contract as `set`: call it on
    /// (or ordered-after) the owning region's tick.
    value
    get(std::string_view output_port_name) const;

    /// Tolerant read: like `get(name)`, but returns `fallback` when the port has
    // no value yet instead of throwing. A crossed output reads monostate until
    // its source region has ticked at least once (a ui tick before the first
    // audio tick, or a fresh program), so a connector that polls it would
    // otherwise hit std::bad_variant_access — a timing artefact, not a bug.
    // A genuine type mismatch still throws.
    template<typename T>
    T
    get_or(std::string_view output_port_name, T fallback) const;

    /// Read back an input port's current value: the last value set, or
    /// `monostate` for a disengaged event port. A level port persists. Same
    /// quiescence contract as `set`/`get`.
    value
    get_input(std::string_view input_port_name) const;

    /// Set an input by its positional index in the global port order
    /// (declaration order from a recipe, span order from a descriptor). This
    /// is the only port identity a nameless graph has.
    void
    set(std::size_t index, value);

    /// Read an output by its positional index in the global port order.
    value
    get(std::size_t index) const;

    /// Read an input by its positional index in the global port order.
    value
    get_input(std::size_t index) const;

    /// The mutable execution storage for one region.
    region_runtime&
    region(clock_tag);

    /// The read-only execution storage for one region.
    const region_runtime&
    region(clock_tag) const;

    /// The user value types this program can carry, deduplicated. The debugger
    /// resolves a value's `user_type` against this list; the kernel never
    /// reads it.
    std::span<const user_type* const>
    types() const noexcept
    {
        return { m_types.data(), m_types.size() };
    }

private:
    friend class engine;

    // Engine-only transfer protocol, not host API: these are private so a
    // `latest` read can only happen inside a region step, within the read
    // window the region announced on entry. Announcing is what withholds every
    // cell retired since the tick began, so a `receive` from outside a step can
    // retain a cell its owner has already reclaimed.
    //
    // Engine fill/drain with explicit epochs (§3.2 versioned snapshot).
    //
    // `send_*` stamps the producer's new epoch (old published + 1, computed by
    // the engine once per tick so all channels of the tick share it) and takes
    // a reference for the channel. A latest channel owns one reference per
    // ring slot and returns the displaced slot's reference when a slot is
    // reused; a queued channel owns the reference until the consumer takes
    // it.
    // `receive` takes the producer's published published_epoch snapshotted once
    // per consumer tick per producer region, so every channel from one
    // producer is read at a single cut, and returns an *owned* value (queued
    // transfers the queue's reference; latest takes one). Returns nullopt when
    // the cut has nothing to show: for queued, an empty or still-future head
    // (normal — the engine stages monostate and moves on); for latest, a
    // snapshot so stale the ring lapped it (the engine re-snapshots fresh and
    // retries the fill so the pair stays on one cut).
    void
    send_queued(node_id, const value&, conc::epoch_id epoch);

    void
    send_latest(node_id, const value&, conc::epoch_id epoch);

    std::optional<value>
    receive(node_id, conc::epoch_id published);

public:
    /// Values dropped by a full queued link (monotonic, program-wide). The L6
    /// capacity precondition is the application's; the counter makes a
    /// violation observable and the library takes no further action.
    std::uint64_t
    dropped_sends() const noexcept
    {
        return m_dropped_sends.load(std::memory_order_relaxed);
    }

    /// Fills that fell back to the lapped-snapshot path: under extreme
    /// scheduling pressure the consumer was lapped by the producer's ring, so
    /// the tick read the newest value per channel instead of one cut. That
    /// weakens the §3.2 cut guarantee *visibly* (a pair may disagree).
    /// Monotonic, program-wide; the engine increments it, hosts and tests read
    /// it.
    std::uint64_t
    lapped_fills() const noexcept
    {
        return m_lapped_fills.load(std::memory_order_relaxed);
    }

    void
    note_lapped_fill() noexcept
    {
        m_lapped_fills.fetch_add(1, std::memory_order_relaxed);
    }

    /// Reclaim retired user cells belonging to one region's home: destroy
    /// their payloads and return their memory to the pool, but only for cells
    /// every region has moved past (the grace period that keeps a lock-free
    /// `latest` reader safe). Bounded work — at most `max_cells` queue pops —
    /// so a ticking region can call it every tick; call it on the region's own
    /// thread. Returns the number of cells reclaimed. A host that stops ticking
    /// a region should call this from its maintenance path.
    std::size_t
    reclaim(clock_tag, std::size_t max_cells);

    /// The same, with the per-tick budget from
    /// `program_options::reclaim_batch`.
    std::size_t
    reclaim(clock_tag);

    /// The region a node belongs to.
    clock_tag
    clock_of(node_id id) const;

private:
    // Slot of a port node inside its region's id list (throws logic_error
    // when the node is not listed — a construction bug, never user input).
    static
    std::size_t
    input_slot(const region_runtime&, node_id);

    static
    std::size_t
    output_slot(const region_runtime&, node_id);

    // Shared tail of both constructors: topology, port validation, region
    // runtimes, links. `crossings` maps sharing-point sources to modes.
    void
    init(const shell::flat_map<node_id, conc::policy>& crossings);

    // Turn every user-typed scan seedsdelay/scan seed (a recipe `closure_cell` value) into
    // a program-owned kernel value, allocated from the owning region's pool at
    // construction — the ticking thread never constructs one. Must run after
    // init(): it needs the regions' homes.
    void
    materialize_seeds();

    void
    construct_region_runtime(clock_tag);

    // True when no region is still reading a window that could touch `cell`.
    bool
    reclaimable(const user_cell*) const noexcept;

    // Owned descriptor storage (plain data). Closure bytes live in m_cells
    // (recipe path, cloned) or are borrowed from the caller's descriptor
    // (span path); port-name bytes are borrowed in both paths.
    std::vector<node_descriptor> m_nodes;
    std::vector<node_label> m_labels; // parallel to m_nodes; descriptive
    std::vector<edge> m_edges;
    std::vector<node_id> m_fan_in;
    std::vector<std::size_t> m_fan_in_offsets;
    std::vector<node_id> m_input_ids;
    std::vector<const char*> m_input_names;
    std::vector<node_id> m_output_ids;
    std::vector<const char*> m_output_names;
    std::vector<std::shared_ptr<void>> m_cells;

    // Materialized user-typed scan seeds seeds, one per user-typed seed in node order.
    // Addresses are handed to `node_descriptor.self`, so the vector is never
    // modified after materialize_seeds() — which is why that function reserves
    // and fills before repointing anything.
    std::vector<value> m_seed_values;

    std::vector<const user_type*> m_types;

    // One stable value home per region (indexed by clock_tag). Addresses are
    // handed to user cells, so the homes live here, in a program that is never
    // moved, not inside a region runtime (which moves with its map).
    std::array<std::unique_ptr<value_home>, clock_tag_count> m_homes {};

    // The program-wide grace clock the homes' cells stamp retirements with.
    // Its address is handed to the homes during construct_region_runtime, once
    // all members exist.
    grace_clock m_grace {};

    std::atomic<std::uint64_t> m_dropped_sends { 0 };
    std::atomic<std::uint64_t> m_lapped_fills { 0 };

    program_options m_options {};

    struct clockr::kernel::topology<descriptor_view> m_topology;

    // Global port directory. Values live in the regions; these stay as the
    // single source of truth for declaration order (positional set/get) and
    // name resolution (named set/get). Private: not part of the contract.
    // Empty on the span path unless the descriptor carries names.
    shell::flat_map<std::string_view, shell::draft::port> m_input_map;
    shell::flat_map<std::string_view, shell::draft::port> m_output_map;

    shell::flat_map<node_id, conc::link<value, conc::policy::queued>> m_links_queued;
    shell::flat_map<node_id, conc::link<value, conc::policy::latest>> m_links_latest;

    shell::flat_map<clockr::kernel::clock_tag, region_runtime> m_region_runtime;
};

} // namespace clockr::kernel

// ############### IMPLEMENTATION ################
namespace clockr::kernel
{

template<typename in_t, typename out_t, typename skeleton_t, std::size_t N>
program::program(const shell::recipe<in_t, out_t, skeleton_t, N>& network, program_options options) :
    m_options { options }
{
    shell::draft draft;
    std::optional<clock_tag> clock;

    shell::instantiate(draft, clock, network);

    // Nodes, cloning stateful closures into program-owned cells (the recipe
    // may die after construction; cf. shell::build for the same mechanism). A
    // user-typed scan seedsdelay/scan seed has no cell record: it is materialized after
    // init(), when the owning region's pool exists — `self` keeps pointing at
    // the recipe until then (nothing ticks before then).
    for (const auto& node : draft.nodes())
    {
        const void* self { node.self };

        if (self != nullptr && node.materialize == nullptr)
        {
            const auto cell_it
            {
                std::find_if(
                    std::begin(draft.cells()),
                    std::end(draft.cells()),
                    [self_ptr = self](const auto& record)
                    {
                        return self_ptr == record.self;
                    }
                )
            };
            assert(cell_it != std::end(draft.cells()));

            m_cells.push_back(cell_it->clone(self));
            self = m_cells.back().get();
        }

        m_nodes.push_back(node_descriptor{ id(node), kernel::clock(node), kind(node), node.tick, self, node.materialize });
    }

    for (const auto& edge : draft.edges())
        m_edges.push_back(edge);

    m_labels.assign(draft.labels().begin(), draft.labels().end());

    // Fan-in tables (same derivation as shell::compile; keep in sync).
    m_fan_in_offsets.assign(m_nodes.size() + 1, 0);

    for (const auto& edge : m_edges)
        ++m_fan_in_offsets[edge.to + 1];

    for (std::size_t i { 1 }; i < m_fan_in_offsets.size(); ++i)
        m_fan_in_offsets[i] += m_fan_in_offsets[i - 1];

    m_fan_in.assign(m_edges.size(), 0);

    {
        std::vector<std::size_t> cursor(m_fan_in_offsets.begin(), m_fan_in_offsets.end() - 1);

        for (const auto& edge : m_edges)
            m_fan_in[cursor[edge.to]++] = edge.from;
    }

    m_input_ids = draft.context.input_ids;
    m_input_names = draft.context.input_names;
    m_output_ids = draft.context.output_ids;
    m_output_names = draft.context.output_names;
    m_input_map = draft.context.input_map;
    m_output_map = draft.context.output_map;
    m_types = draft.context.used_types;

    init(draft.context.crossing_map);
    materialize_seeds();
}

template<typename T, typename... Args>
requires std::constructible_from<T, Args...>
void
program::set(std::string_view input_port_name, Args&&... args)
{
    static_assert(user_value_c<T>, "program::set<T> is for user structs; scalar ports use set(name, value)");

    const auto maybe_port { m_input_map.value_for_key(input_port_name) };

    if (!maybe_port)
        throw std::runtime_error{ "Input port not found: " + std::string { input_port_name } };

    auto& runtime { m_region_runtime[maybe_port->region] };

    assert(!runtime.in_step.load(std::memory_order_relaxed)
           && "input port set while its region was ticking: set must be quiescent with the region tick");

    const auto slot { input_slot(runtime, maybe_port->node_id) };

    const value constructed { make_user_value<T>(runtime.home, std::forward<Args>(args)...) };

    transfer_assign(runtime.inputs[slot], constructed);
    runtime.input_dirty[slot] = std::byte{ 1 };
}

template<typename T>
T
program::get_or(std::string_view output_port_name, T fallback) const
{
    const value v { get(output_port_name) };

    if (std::holds_alternative<std::monostate>(v))
        return fallback;

    using U = std::remove_cvref_t<T>;

    // User structs (and optionals of them) reconstruct through the shell, as
    // in `kernel::get`; scalars keep the strict read. Monostate was handled
    // above, so a remaining mismatch is a genuine type bug and throws.
    if constexpr (user_value_c<U> || is_optional_v<U>)
        return shell::from_value<T>(v);
    else
        return std::get<T>(v);
}

template<typename T>
constexpr
T
get(const program& program, std::string_view output_port_name)
{
    using U = std::remove_cvref_t<T>;

    // User structs (and optionals of them) are read through the shell, which
    // knows how to reconstruct them and copies out a value; scalars keep the
    // direct strict read.
    if constexpr (user_value_c<U> || is_optional_v<U>)
        return shell::from_value<T>(program.get(output_port_name));
    else
        return std::get<T>(program.get(output_port_name));
}

template<typename T>
T
get_input(const program& program, std::string_view input_port_name)
{
    using U = std::remove_cvref_t<T>;

    if constexpr (user_value_c<U> || is_optional_v<U>)
        return shell::from_value<T>(program.get_input(input_port_name));
    else
        return std::get<T>(program.get_input(input_port_name));
}

} // namespace clockr::kernel
