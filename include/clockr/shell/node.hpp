#pragma once

/// @file
/// The shell's by-value recipe arena: `node_ref` indices and the `node`
/// variant that stores each operator's data. Internal to `recipe`; callers
/// compose recipes through the operators, never through `node` directly.

#include "clockr/shell/closure_cell.hpp"
#include "clockr/conc/link.hpp"
#include "clockr/shell/draft.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace clockr::shell
{

struct node;

// An AST child reference: an *index* into the owning `recipe`'s arena, never a
// pointer. Storing the recipe tree by value (a flat `std::array<node>`)
// makes `recipe` copyable and constant-initializable; indices stay valid across
// copies and moves, where absolute pointers into a by-value member would not.
using node_ref = std::size_t;

// Upper bound on the fan-in of the structural `zip`/`parallel` nodes. Audio
// patches stay well under this; raise it if a network needs more branches —
// but note every AST node embeds a kids array of this size (via zip_t /
// parallel_t), so it sets the ceiling for sizeof(node) alongside
// closure_cell. Wider fan-in nests instead: zip(zip(a..h), i) flattens to the
// same leaf order as a flat 9-wide zip (L8).
inline constexpr std::size_t max_branches { 8 };

// ---------------------------------------------------------------------------
// the recipe AST — tagged (variant) nodes, one alternative per operator.
//
// A node is plain value data: no virtuals, so the whole tree is
// constant-representable. The single dispatch point is the
// std::visit walker at the bottom of this file; each operator's node type
// contributes one variant alternative plus its own `first_anchor` /
// `instantiate` members. Children are referenced by `node_ref` into the
// enclosing `recipe`'s arena, so the walker threads an `arena` view through every
// call to resolve them.
// ---------------------------------------------------------------------------

namespace detail
{

struct anchor
{
    enum class kind
    {
        none,
        input,
        output,
    };

    kind k { kind::none };
    kernel::clock_tag tag { kernel::clock_tag::audio };

    explicit
    constexpr
    operator bool() const noexcept
    {
        return k != kind::none;
    }
};

// Anchor a fragment that was reached with no clock established. Every recipe
// except `parallel`/`zip` begins its instantiation with this: if `clock` is
// already set the fragment simply inherits it; otherwise the fragment looks
// ahead for its first port — the walk-order `first_anchor()` — and seeds the
// walk from that clock, so interior nodes know where they run.
//
// Only an *input* can seed a clock: data enters a fragment at a tagged input,
// and every constructible fragment begins with one. An output tag is a
// *check*, not a seed (a head-less "constant generator" has no data source,
// so its clock cannot be derived forward). A fragment with no anchor anywhere
// is dead code — reject it at build time rather than guess.
template<typename node_t>
constexpr
void
seed_clock(node_t& r, std::optional<kernel::clock_tag>& clock, std::span<const node> arena);

inline
constexpr
detail::anchor
first_anchor(const node&, std::span<const node> arena);

inline
constexpr
std::vector<kernel::node_id>
instantiate(const node&, std::span<const node> arena, draft& d, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs);

inline
constexpr
kernel::value
pass_through(const void*, const kernel::tick_context& ctx, std::span<const kernel::value> inputs);

// A delay/scan seed whose type is a user struct: the recipe stores the seed by
// value (`closure_cell`), and the *program* materializes it into a kernel value
// once, at construction, with the owning region's pool — never on the ticking
// thread. After that the node's `self` points at that value, exactly like a
// scalar seed, and the delay tick just reads it.
template<typename T>
kernel::value
materialize_user_seed(const void* self, const kernel::value_home* home)
{
    return kernel::make_user_value<T>(home, *static_cast<const T*>(self));
}

// The materialize function for a seed type: null for scalars (their
// `kernel::value` is cloned into program-owned storage the usual way).
template<typename T>
constexpr
kernel::node_descriptor::materialize_fn
materialize_for() noexcept
{
    if constexpr (kernel::user_value_c<T>)
        return &materialize_user_seed<T>;
    else
        return nullptr;
}

// The recipe address a delay/scan seed lives at: the `kernel::value` itself for
// scalars, the `closure_cell` for user structs. It becomes `node_descriptor.self`.
constexpr
const void*
init_self(const std::variant<kernel::value, closure_cell>& init) noexcept
{
    if (const auto* value { std::get_if<kernel::value>(&init) })
        return value;

    return std::get<closure_cell>(init).self();
}

// The clone function for a scalar seed: the program owns a heap `kernel::value`
// copy. User seeds are materialized instead (see materialize_for), so this is
// only ever called for the `kernel::value` alternative.
inline
std::shared_ptr<void>
clone_graph_value(const void* self)
{
    return std::shared_ptr<void>(new kernel::value(*static_cast<const kernel::value*>(self)));
}

// Static operator names for node labels. One literal per graph-emitting
// operator (structural ones — compose/zip/parallel/share/at — emit no node
// of their own, so they never label anything).
inline constexpr const char* label_lift { "lift" };
inline constexpr const char* label_scan { "scan" };
inline constexpr const char* label_delay { "delay" };
inline constexpr const char* label_input { "input" };
inline constexpr const char* label_output { "output" };

} // namespace detail

// at: the value of a network as seen on another clock: the crossing and its policy.
class at_t
{
public:
    constexpr
    at_t(node_ref source, kernel::clock_tag clock, conc::policy policy) :
        m_source { source },
        m_clock { clock },
        m_policy { policy }
    {}

    constexpr
    bool
    operator==(const at_t& rhs) const
    {
        return m_source == rhs.m_source && m_clock == rhs.m_clock && m_policy == rhs.m_policy;
    }

    constexpr
    node_ref
    source() const noexcept
    {
        return m_source;
    }

    constexpr
    kernel::clock_tag
    clock() const noexcept
    {
        return m_clock;
    }

    constexpr
    conc::policy
    policy() const noexcept
    {
        return m_policy;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node>) const
    {
        return
        {
            detail::anchor::kind::input, // Resamplers behave like inputs on their tagged networks.
            m_clock
        };
    }

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    node_ref m_source;
    kernel::clock_tag m_clock;
    conc::policy m_policy;
};

// lift: a pure function over its inputs, lowered to a single node. `arity`
// is the leaf count of the (possibly pair) input wire — 1 for a plain input,
// N for a nested pair — so one node kind serves lift of every arity.
class lift_t
{
public:
    constexpr lift_t() = default;

    constexpr
    lift_t(kernel::node_descriptor::tick_fn tick, detail::closure_cell owner, std::size_t arity, const kernel::user_type* type = nullptr) :
        m_tick { tick },
        m_owner { std::move(owner) },
        m_arity { arity },
        m_type { type }
    {}

    constexpr bool operator==(const lift_t& other) const
    {
        return m_tick == other.m_tick && m_owner == other.m_owner && m_arity == other.m_arity
            && m_type == other.m_type;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node>) const
    {
        return {};
    }

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    kernel::node_descriptor::tick_fn m_tick {};
    detail::closure_cell m_owner {};
    [[maybe_unused]] std::size_t m_arity { 0 };
    const kernel::user_type* m_type { nullptr };
};

class compose_t
{
public:
    constexpr
    compose_t(node_ref first, node_ref second) :
        m_first { first },
        m_second { second }
    {}

    constexpr bool operator==(const compose_t& other) const
    {
        return m_first == other.m_first && m_second == other.m_second;
    }

    constexpr
    node_ref
    first() const noexcept
    {
        return m_first;
    }

    constexpr
    node_ref
    second() const noexcept
    {
        return m_second;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node> arena) const;

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    node_ref m_first;
    node_ref m_second;
};

class input_t
{
public:
    constexpr
    input_t(kernel::clock_tag clock, std::string_view port_name, bool event, const kernel::user_type* type = nullptr) :
        m_clock { clock },
        m_port_name { port_name },
        m_event { event },
        m_type { type }
    {}

    constexpr bool operator==(const input_t& other) const
    {
        return m_clock == other.m_clock && m_port_name == other.m_port_name
            && m_event == other.m_event && m_type == other.m_type;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node>) const
    {
        return { detail::anchor::kind::input, m_clock };
    }

    constexpr
    kernel::clock_tag
    clock() const noexcept
    {
        return m_clock;
    }

    constexpr
    std::string_view
    port_name() const noexcept
    {
        return m_port_name;
    }

    // True for fire-and-forget (optional-typed) ports: the engine reverts
    // their storage to disengaged after the consuming tick. Plain ports
    // persist whatever was last set.
    constexpr
    bool
    event() const noexcept
    {
        return m_event;
    }

    constexpr
    const kernel::user_type*
    type() const noexcept
    {
        return m_type;
    }

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    kernel::clock_tag m_clock;
    std::string_view m_port_name;
    bool m_event;
    const kernel::user_type* m_type { nullptr };
};

// output: a sink port anchored on `thread` — its value is read on that clock.
// The tag doubles as a consistency check (see the error message in
// instantiate below): a chain feeding an output on a different clock is a
// build-time error (no implicit cross-clock resampling).
class output_t
{
public:
    constexpr
    output_t(kernel::clock_tag clock, std::string_view port_name, const kernel::user_type* type = nullptr) :
        m_clock { clock },
        m_port_name { port_name },
        m_type { type }
    {}

    constexpr bool operator==(const output_t& other) const
    {
        return m_clock == other.m_clock && m_port_name == other.m_port_name
            && m_type == other.m_type;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node>) const
    {
        return { detail::anchor::kind::output, m_clock };
    }

    constexpr
    kernel::clock_tag
    clock() const noexcept
    {
        return m_clock;
    }

    constexpr
    std::string_view
    port_name() const noexcept
    {
        return m_port_name;
    }

    constexpr
    const kernel::user_type*
    type() const noexcept
    {
        return m_type;
    }

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    kernel::clock_tag m_clock;
    std::string_view m_port_name;
    const kernel::user_type* m_type { nullptr };
};

// share: fan-out — the inner fragment is instantiated (and evaluated) once per
// tick, then its output feeds every consumer. Memoization keys on a stable id
// carried by value, so that copying/concat-ing a shared `recipe` preserves the
// identity across deep copies (a by-value arena would otherwise mint a fresh
// distinct `share_t` per use, breaking dedup). `m_id` is minted by `share()`
// from the call site's `std::source_location` (a compile-time constant, unique
// per source token) — see operator/share.hpp — so `share` stays constexpr and
// the whole operator set remains compile-able.
class share_t
{
public:
    constexpr
    explicit share_t(node_ref inner) :
        share_t(inner, 0)
    {}

    constexpr
    share_t(node_ref inner, std::uint64_t id) :
        m_inner { inner },
        m_id { id }
    {}

    constexpr bool operator==(const share_t& other) const
    {
        return m_inner == other.m_inner && m_id == other.m_id;
    }

    constexpr
    node_ref
    inner() const noexcept
    {
        return m_inner;
    }

    constexpr
    std::uint64_t
    id() const noexcept
    {
        return m_id;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node> arena) const;

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    node_ref m_inner;
    std::uint64_t m_id;
};

// scan: accumulates state across ticks. A delay node seeds the state and a
// scanner node folds it with the input each tick (the engine drives the delay).
class scan_t
{
public:
    constexpr
    scan_t(
        std::variant<kernel::value, detail::closure_cell> init,
        kernel::node_descriptor::materialize_fn materialize,
        kernel::node_descriptor::tick_fn tick,
        detail::closure_cell owner,
        const kernel::user_type* type = nullptr) :
        m_init { std::move(init) },
        m_materialize { materialize },
        m_tick { tick },
        m_owner { std::move(owner) },
        m_type { type }
    {}

    constexpr bool operator==(const scan_t& other) const
    {
        return m_init == other.m_init && m_materialize == other.m_materialize
            && m_tick == other.m_tick && m_owner == other.m_owner && m_type == other.m_type;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node>) const
    {
        return {};
    }

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    std::variant<kernel::value, detail::closure_cell> m_init;
    kernel::node_descriptor::materialize_fn m_materialize { nullptr };
    kernel::node_descriptor::tick_fn m_tick {};
    detail::closure_cell m_owner {};
    const kernel::user_type* m_type { nullptr };
};

// delay: a unit delay seeded with an initial value. Outputs the seed on the
// first tick, then the previous tick's input — the Kahn-seed half of scan,
// exposed standalone so feedback loops can carry their own state. The engine
// drives the delay state (evaluate_nodes reads it, settle writes it); the
// tick only ever produces the seed, via detail::delay_tick in draft.hpp.
class delay_t
{
public:
    constexpr
    explicit delay_t(
        std::variant<kernel::value, detail::closure_cell> init,
        kernel::node_descriptor::materialize_fn materialize,
        const kernel::user_type* type = nullptr) :
        m_init { std::move(init) },
        m_materialize { materialize },
        m_type { type }
    {}

    constexpr bool operator==(const delay_t& other) const
    {
        return m_init == other.m_init && m_materialize == other.m_materialize && m_type == other.m_type;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node>) const
    {
        return {};
    }

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    std::variant<kernel::value, detail::closure_cell> m_init;
    kernel::node_descriptor::materialize_fn m_materialize { nullptr };
    const kernel::user_type* m_type { nullptr };
};

// zip: N branches sharing one upstream; the output type is the pair of the
// branches' outputs (compile-time only — it never flows as a kernel::value).
class zip_t
{
public:
    constexpr
    explicit zip_t(std::array<node_ref, max_branches> networks, std::size_t count) :
        m_networks { networks },
        m_count { count }
    {}

    constexpr bool operator==(const zip_t& other) const
    {
        return m_networks == other.m_networks && m_count == other.m_count;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node> arena) const;

    constexpr
    std::span<const node_ref>
    networks() const noexcept;

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const;

private:
    std::array<node_ref, max_branches> m_networks {};
    std::size_t m_count { 0 };
};

// parallel: N independent chains with N independent outputs. Emits no node of
// its own and establishes no clock — each child anchors itself from its own
// ports, the incoming clock being only a default.
class parallel_t
{
public:
    constexpr
    explicit parallel_t(std::array<node_ref, max_branches> networks, std::size_t count) :
        m_networks { networks },
        m_count { count }
    {}

    constexpr bool operator==(const parallel_t& other) const
    {
        return m_networks == other.m_networks && m_count == other.m_count;
    }

    constexpr
    detail::anchor
    first_anchor(std::span<const node> arena) const;

    constexpr
    std::span<const node_ref>
    networks() const noexcept;

    constexpr
    std::vector<kernel::node_id>
    instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, [[maybe_unused]] std::span<const kernel::node_id> inputs) const;

private:
    std::array<node_ref, max_branches> m_networks {};
    std::size_t m_count { 0 };
};

// node — the erased recipe. Every operator's node type is one alternative.
struct node
{
    std::variant<
        lift_t,
        compose_t,
        input_t,
        output_t,
        share_t,
        scan_t,
        delay_t,
        zip_t,
        parallel_t,
        at_t
    > v;

    constexpr bool operator==(const node& other) const
    {
        return v == other.v;
    }
};

// --- detail helpers (constexpr) ---------------------------------------------
//
// Defined after `node` is complete but *before* the node member bodies
// below, so the member `instantiate` bodies can constant-evaluate their calls
// to these (a constexpr call needs the callee's definition already in scope).
// The std::visit dispatch plus these helpers are the walker's whole control
// flow.

namespace detail
{

template<typename node_t>
constexpr
void
seed_clock(node_t& r, std::optional<kernel::clock_tag>& clock, std::span<const node> arena)
{
    if (clock)
        return;

    const auto a { r.first_anchor(arena) };

    if (a.k != anchor::kind::input)
        throw std::runtime_error(
            "no clock anchor: a fragment's clock is established where data enters — a tagged "
            "input. This fragment does not begin with one, so its clock cannot be derived; "
            "start it with input<T>(clock, ...), at<clock, mode>(...), or feed it from an already-anchored fragment");

    clock = a.tag;
}

inline
constexpr
detail::anchor
first_anchor(const node& root, std::span<const node> arena)
{
    return std::visit([&](const auto& n) { return n.first_anchor(arena); }, root.v);
}

// ---------------------------------------------------------------------------
// the walker — std::visit dispatch over node
// ---------------------------------------------------------------------------
inline
constexpr
std::vector<kernel::node_id>
instantiate(const node& root, std::span<const node> arena, draft& d, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs)
{
    return std::visit([&](const auto& n) { return n.instantiate(arena, d, clock, inputs); }, root.v);
}

inline
constexpr
const char*
tag_name(kernel::clock_tag t)
{
    switch (t)
    {
        case kernel::clock_tag::audio: return "audio";
        case kernel::clock_tag::ui:    return "ui";
        case kernel::clock_tag::worker: return "worker";
        default: return "?";
    }
}

inline
constexpr
kernel::value
pass_through(const void*, [[maybe_unused]] const kernel::tick_context& ctx, std::span<const kernel::value> inputs)
{
    if (inputs.empty())
        return kernel::value { std::monostate {} };

    // An output node's tick is the identity: it forwards its fan-in value to
    // the tick's value slot, which owns one reference. The port's copy is a
    // borrow, so retaining here is what gives the slot its own reference.
    kernel::retain(inputs.front());
    return inputs.front();
}

// The port name of an input-typed node, or nullopt for any other node. Used by
// `share` to catch one source-located share spanning two different input ports
// (a helper parameterized by port name).
constexpr
std::optional<std::string_view>
input_port_name(const node& root) noexcept
{
    return std::visit(
        [](const auto& n) -> std::optional<std::string_view>
        {
            using T = std::decay_t<decltype(n)>;

            if constexpr (std::is_same_v<T, input_t>)
                return n.port_name();
            else
                return std::nullopt;
        },
        root.v);
}

} // namespace detail

// --- node member definitions (out-of-line) ----------------------------------
//
// These bodies subscript `std::span<const node>`, which requires the element
// type to be complete. They must therefore live *after* `node` is defined
// (the node classes above only declare them); defining them inline would trigger
// `span::operator[]` on an incomplete type.

inline
constexpr
std::vector<kernel::node_id>
at_t::instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{
    // at<> must actually cross clocks — same-clock resampling is a no-op and
    // almost certainly a bug. valid.hpp:129 reports this as validity::same_clock
    // for compile-time use; we duplicate the check here for the runtime path so
    // shell::build / shell::instantiate throw without needing to call valid().
    // We must check against the source fragment's clock, not
    // the incoming clock which is stale after the reset.
    const auto incoming { clock };

    clock = std::nullopt; // Remove current clock, delegating m_source to seed it against our first_anchor.

    const auto result { detail::instantiate(arena[m_source], arena, draft, clock, inputs) };

    // If the seeded clock already equals the target, the crossing is redundant.
    if (clock && *clock == m_clock)
        throw std::runtime_error(
            "at<> on same clock as source (" + std::string{ detail::tag_name(m_clock) }
            + "): resampling within one clock is a no-op; remove the at<>");

    // Also catch the stale-incoming case (e.g. at<ui> in a ui context with
    // an source that doesn't anchor) for completeness.
    if (incoming && *incoming == m_clock && !clock)
        throw std::runtime_error(
            "at<> on same clock as surrounding (" + std::string{ detail::tag_name(m_clock) }
            + "): resampling within one clock is a no-op; remove the at<>");

    for (const auto node_id : result)
        draft.context.crossing_map.try_emplace(node_id, m_policy);

    // Downstream of the crossing runs on the target clock. This is not
    // redundant: the source re-seeded `clock` with *its* clock (which the
    // checks above proved differs from the target), so the walker must be
    // re-anchored here before the continuation is instantiated.
    clock = m_clock;

    return result;
}

inline
constexpr
std::vector<kernel::node_id>
lift_t::instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{
    detail::seed_clock(*this, clock, {});

    if (inputs.size() != m_arity)
        throw std::runtime_error("lift networks have exactly their declared fan-in arity");

    draft.register_type(m_type);

    const void* self { nullptr };
    if (m_owner.has_value())
    {
        self = m_owner.self();
        draft.register_cell(self, m_owner.clone_function());
    }

    const auto id { draft.add_node(*clock, kernel::node_kind::internal, m_tick, self, { detail::label_lift, nullptr }) };

    for (const auto in : inputs)
        draft.connect(in, id);

    return { id };
}

inline
constexpr
detail::anchor
compose_t::first_anchor(std::span<const node> arena) const
{
    const auto a { detail::first_anchor(arena[m_first], arena) };
    return a ? a : detail::first_anchor(arena[m_second], arena);
}

inline
constexpr
std::vector<kernel::node_id>
compose_t::instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{
    detail::seed_clock(*this, clock, arena);

    const auto first_outputs { detail::instantiate(arena[m_first], arena, draft, clock, inputs) };
    return detail::instantiate(arena[m_second], arena, draft, clock, first_outputs);
}

inline
constexpr
std::vector<kernel::node_id>
input_t::instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{
    if (!inputs.empty())
        throw std::runtime_error("input ports have no upstream");

    detail::seed_clock(*this, clock, {});

    if (*clock != m_clock)
        throw std::runtime_error(
            "Input port \"" + std::string{ m_port_name } + "\" is on " + detail::tag_name(m_clock)
            + " but its surroundings are on " + detail::tag_name(*clock)
            + ": cross-clock flow needs an explicit crossing (at<>)");

    if (draft.context.input_map.contains(m_port_name))
        throw std::runtime_error(
            "Input port name already exists: " + std::string{ m_port_name }
            + " (to fan one port out to several consumers, bind it once with shell::share(...))");

    draft.register_type(m_type);

    const auto id { draft.add_node(m_clock, kernel::node_kind::input, &detail::pass_through, nullptr, { detail::label_input, m_port_name.data() }) };

    draft.context.input_ids.push_back(id);
    draft.context.input_names.push_back(m_port_name.data());
    draft.context.input_map.try_emplace(m_port_name, draft::port{ m_clock, id, m_event });

    return { id };
}

inline
constexpr
std::vector<kernel::node_id>
output_t::instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{
    if (inputs.size() != 1)
        throw std::runtime_error("output ports have exactly one input");

    detail::seed_clock(*this, clock, {});

    if (*clock != m_clock)
        throw std::runtime_error(
            "Output port \"" + std::string{ m_port_name } + "\" is on " + detail::tag_name(m_clock)
            + " but its sources are on " + detail::tag_name(*clock)
            + ": crossing a clock requires an explicit crossing (at<>)");

    if (draft.context.output_map.contains(m_port_name))
        throw std::runtime_error("Output port name already exists: " + std::string{ m_port_name });

    draft.register_type(m_type);

    const auto id { draft.add_node(m_clock, kernel::node_kind::output, &detail::pass_through, nullptr, { detail::label_output, m_port_name.data() }) };

    for (const auto in : inputs)
        draft.connect(in, id);

    draft.context.output_ids.push_back(id);
    draft.context.output_names.push_back(m_port_name.data());
    draft.context.output_map.try_emplace(m_port_name, draft::port{ m_clock, id });

    return { id };
}

inline
constexpr
std::vector<kernel::node_id>
scan_t::instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{
    if (inputs.size() != 1)
        throw std::runtime_error("scan networks have exactly one input");

    detail::seed_clock(*this, clock, {});

    draft.register_type(m_type);

    const void* const delay_self { detail::init_self(m_init) };

    if (m_materialize == nullptr)
        draft.register_cell(delay_self, &detail::clone_graph_value);

    const auto delay_id { draft.add_delay(*clock, delay_self, m_materialize, { detail::label_scan, nullptr }) };

    const void* scanner_self { nullptr };
    if (m_owner.has_value())
    {
        scanner_self = m_owner.self();
        draft.register_cell(scanner_self, m_owner.clone_function());
    }
    const auto scanner_id { draft.add_node(*clock, kernel::node_kind::internal, m_tick, scanner_self, { detail::label_scan, nullptr }) };

    draft.connect(delay_id, scanner_id);
    draft.connect(inputs[0], scanner_id);
    draft.connect(scanner_id, delay_id);

    return { scanner_id };
}

inline
constexpr
std::vector<kernel::node_id>
delay_t::instantiate(std::span<const node>, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{
    if (inputs.size() != 1)
        throw std::runtime_error("delay networks have exactly one input");

    detail::seed_clock(*this, clock, {});

    draft.register_type(m_type);

    // The delay half of scan_t::instantiate, standalone: the seed lives in a
    // cell (borrowed from this recipe node at compile time, cloned at build),
    // the engine reads it on the first tick and the previous input after.
    const void* const delay_self { detail::init_self(m_init) };

    if (m_materialize == nullptr)
        draft.register_cell(delay_self, &detail::clone_graph_value);

    const auto delay_id { draft.add_delay(*clock, delay_self, m_materialize, { detail::label_delay, nullptr }) };

    draft.connect(inputs[0], delay_id);

    return { delay_id };
}

inline
constexpr
detail::anchor
share_t::first_anchor(std::span<const node> arena) const
{
    return detail::first_anchor(arena[m_inner], arena);
}

inline
constexpr
std::vector<kernel::node_id>
share_t::instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{
    detail::seed_clock(*this, clock, arena);

    // Key memoization on the stable by-value id carried from `share()`: the
    // same shared `recipe` copied into many places keeps one id, so its inner
    // fragment is emitted once per clock, while two distinct shares (or one
    // share on two clocks) emit separately.
    const auto key { std::make_pair(m_id, clock) };
    const auto inner_input { detail::input_port_name(arena[m_inner]) };

    // A share id is nominal: it names one `share(...)` source location. A
    // helper parameterized by port name (or otherwise reusing the same share
    // token for two fragments) would make the memo return the first port's
    // outputs for the second, silently dropping a port. Reject it loudly.
    if (const auto* seen { draft.context.shared_input_names.value_for_key(key) })
    {
        if (inner_input && *seen != *inner_input)
            throw std::runtime_error(
                "share() at one source location was used with two different input ports (\""
                + std::string { *seen } + "\" and \"" + std::string { *inner_input }
                + "\"): a share's identity is its source location, so a helper "
                "parameterized by port name collapses the ports onto one. Give each "
                "port its own share() call site.");
    }

    if (!draft.context.shared_nodes.contains(key))
    {
        if (inner_input)
            draft.context.shared_input_names.try_emplace(key, *inner_input);

        draft.context.shared_nodes.try_emplace(key, detail::instantiate(arena[m_inner], arena, draft, clock, inputs));
    }

    return draft.context.shared_nodes[key];
}

inline
constexpr
detail::anchor
zip_t::first_anchor(std::span<const node> arena) const
{
    for (const auto child : networks())
        if (const auto a { detail::first_anchor(arena[child], arena) }; a)
            return a;

    return {};
}

inline
constexpr
std::span<const node_ref>
zip_t::networks() const noexcept
{
    return std::span<const node_ref>{ m_networks.data(), m_count };
}

inline
constexpr
std::vector<kernel::node_id>
zip_t::instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, std::span<const kernel::node_id> inputs) const
{ // Instantiate each branch and concatenate its output ids, in zip order.
   // `inputs` is the shared upstream (the zip's recipe type parameter X): it is
   // fanned out to every branch, matching shell::zip(recipe<X, In1>, recipe<X, In2>).
   // Branches work on a copy of the clock, so a branch-local anchor never
   // leaks into the zip's own continuation.
    std::vector<kernel::node_id> outputs;

    for (const auto child : networks())
    {
        auto branch { clock };
        const auto network_outputs { detail::instantiate(arena[child], arena, draft, branch, inputs) };
        outputs.insert(std::end(outputs), std::begin(network_outputs), std::end(network_outputs));
    }

    return outputs;
}

inline
constexpr
detail::anchor
parallel_t::first_anchor(std::span<const node> arena) const
{
    for (const auto child : networks())
        if (const auto a { detail::first_anchor(arena[child], arena) }; a)
            return a;

    return {};
}

inline
constexpr
std::span<const node_ref>
parallel_t::networks() const noexcept
{
    return std::span<const node_ref>{ m_networks.data(), m_count };
}

inline
constexpr
std::vector<kernel::node_id>
parallel_t::instantiate(std::span<const node> arena, draft& draft, std::optional<kernel::clock_tag>& clock, [[maybe_unused]] std::span<const kernel::node_id> inputs) const
{
    if (!inputs.empty())
        throw std::runtime_error("parallel networks are self-sufficient, don't take any inputs");

    std::vector<kernel::node_id> outputs;

    for (const auto child : networks())
    {
        auto branch { clock };
        const auto network_outputs { detail::instantiate(arena[child], arena, draft, branch, {}) };
        outputs.insert(std::end(outputs), std::begin(network_outputs), std::end(network_outputs));
    }

    return outputs;
}

namespace detail
{

// --- arena assembly helpers (constexpr) -------------------------------------
//
// Operators build a network's flat arena by concatenating their children's
// arenas (shifting each child's root index by its offset) and appending one new
// node. These are the only place the recursive tree is flattened into an array.

template<std::size_t N>
constexpr
std::array<node, N + 1>
append_node(const std::array<node, N>& arena, node value)
{
    std::array<node, N + 1> result {};
    for (std::size_t i { 0 }; i < N; ++i)
        result[i] = arena[i];
    result[N] = std::move(value);
    return result;
}

// Shift every `node_ref` inside a node by `offset`. When child arenas are
// concatenated into one flat array, a child that lived at index `i` in its own
// arena must point at `i + offset` afterwards, or the walker would resolve its
// children against the wrong arena region. Leaf nodes (lift/input/output/scan)
// carry no `node_ref` children and are returned unchanged.
inline
constexpr
node
shift_indices(const node& n, node_ref offset)
{
    return std::visit(
        [offset](const auto& child) -> node
        {
            using T = std::decay_t<decltype(child)>;

            if constexpr (std::is_same_v<T, compose_t>)
                return node { compose_t(child.first() + offset, child.second() + offset) };
            else if constexpr (std::is_same_v<T, share_t>)
                return node { share_t(child.inner() + offset, child.id()) };
            else if constexpr (std::is_same_v<T, at_t>)
                return node { at_t(child.source() + offset, child.clock(), child.policy()) };
            else if constexpr (std::is_same_v<T, zip_t> || std::is_same_v<T, parallel_t>)
            {
                const auto nets { child.networks() };
                std::array<node_ref, max_branches> shifted {};
                for (std::size_t i { 0 }; i < nets.size(); ++i)
                    shifted[i] = nets[i] + offset;

                if constexpr (std::is_same_v<T, zip_t>)
                    return node { zip_t(shifted, nets.size()) };
                else
                    return node { parallel_t(shifted, nets.size()) };
            }
            else
                return node { child };
        },
        n.v
    );
}

template<std::size_t NA, std::size_t NB>
constexpr
std::array<node, NA + NB>
concat2(const std::array<node, NA>& a, const std::array<node, NB>& b)
{
    std::array<node, NA + NB> result {};
    for (std::size_t i { 0 }; i < NA; ++i)
        result[i] = a[i];
    for (std::size_t i { 0 }; i < NB; ++i)
        result[NA + i] = detail::shift_indices(b[i], NA);
    return result;
}

// Concatenate any number of arenas (one per branch) into one flat array. The
// per-arena sizes are deduced from the array arguments, so no runtime size is
// needed. Each subsequent arena is shifted by the running offset so its
// `node_ref` children keep pointing at the right nodes.
template<std::size_t... Ns>
constexpr
std::array<node, (Ns + ...)>
concat_arenas(const std::array<node, Ns>&... arenas)
{
    std::array<node, (Ns + ...)> result {};
    std::size_t offset { 0 };

    auto copy
    {
        [&](const auto& a)
        {
            for (std::size_t i { 0 }; i < a.size(); ++i)
                result[offset + i] = shift_indices(a[i], offset);
            offset += a.size();
        }
    };

    (copy(arenas), ...);

    return result;
}

} // namespace detail

} // namespace clockr::shell
