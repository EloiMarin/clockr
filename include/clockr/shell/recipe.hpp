#pragma once

#include "clockr/shell/draft.hpp"
#include "clockr/shell/node.hpp"
#include "clockr/shell/skeleton.hpp"

#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/value.hpp"

#include <cassert>
#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

// recipe — signal networks as composable *recipes* (see doc/explanation/frp-decisions.md, Shape C).
//
// An `recipe<in, out>` is not connected to any runtime: it is an immutable,
// by-value description of a subgraph (a flat `node` arena). Operators (`lift`, `operator>>`,
// `input`, `output`) build bigger recipes from smaller ones. Only when
// `instantiate()` is called does a recipe emit nodes and edges into a
// `kernel::graph` — from then on, evaluation is entirely the engine's job and
// the recipe can be discarded.
//
// Threads are anchored at boundaries only (doc/explanation/frp-decisions.md §5, Shape C):
// input and output ports name their clock, and everything between two
// anchors — every interior node — inherits the clock of the nearest upstream
// anchor. There is no "ambient" clock: no build parameter, no `on`
// operator. `build(network)` derives the whole clock structure from the
// ports themselves.
//
// The layering is deliberate: `recipe` is the typed shell, `graph` stays the
// erased kernel. Types are checked at composition time (compile time);
// `kernel::value` remains the only type that flows at runtime.

namespace clockr::shell
{

// ---------------------------------------------------------------------------
// value conversion: typed shell <-> erased kernel
//
// Writing is permissive (any integral becomes int64_t, any floating point
// becomes double) so natural C++ literals just work. Reading is strict
// (`std::get<T>`): the kernel stores exactly the variant alternatives, and a
// mismatch is a bug, not a conversion opportunity — it throws
// std::bad_variant_access.
//
// Absence is spelled std::optional<T>: a disengaged optional erases to the
// monostate "no event" marker, an engaged one converts its payload with the
// same rules as a bare value. Reading mirrors it, so from_value over an
// optional is total over everything the shell can deliver (T or monostate)
// while staying strict on genuine mismatches. This is what lets an event
// crossing be typed without infecting the interior: the head lift matches on
// the optional and everything downstream composes on plain T again.
// ---------------------------------------------------------------------------

/// True when `T` is `std::optional<...>`: the shell's spelling for an event
/// (absence) at a port.
template<typename T>
struct is_optional : std::false_type {};

template<typename T>
struct is_optional<std::optional<T>> : std::true_type {};

/// Convenience variable template for `is_optional`.
template<typename T>
constexpr bool is_optional_v = is_optional<T>::value;

/// Convert a typed shell value to the erased `kernel::value`.
///
/// Writing is permissive: integrals widen to `int64_t`, floating point to
/// `double`, and an optional erases to `monostate` when disengaged. Reading
/// (`from_value`) is strict: it throws on a genuine mismatch, because the
/// kernel stores exactly the variant alternatives.
constexpr
inline kernel::value
to_value(std::monostate v) noexcept
{
    return v;
}

constexpr
inline kernel::value
to_value(bool v) noexcept
{
    return v;
}

constexpr
inline kernel::value
to_value(std::int64_t v) noexcept
{
    return v;
}

constexpr
inline kernel::value
to_value(double v) noexcept
{
    return v;
}

template<typename T>
requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, std::int64_t>)
constexpr
kernel::value
to_value(T v) noexcept
{
    return static_cast<std::int64_t>(v);
}

template<typename T>
requires std::floating_point<T> && (!std::same_as<T, double>)
constexpr
kernel::value
to_value(T v) noexcept
{
    return static_cast<double>(v);
}

template<typename T>
constexpr
kernel::value
to_value(const std::optional<T>& v) noexcept
{
    static_assert(!is_optional_v<T>, "nested optionals are not port types - flatten them");
    static_assert(!std::same_as<T, std::monostate>, "optional<monostate> is not a port type - use monostate");

    return v ? to_value(*v) : kernel::value{std::monostate{}};
}

constexpr
inline kernel::value
to_value(std::nullopt_t) noexcept
{
    return kernel::value{std::monostate{}};
}

// Reading a user struct borrows the payload: an inline value points into the
// variant, a ref-counted one into its pool cell. The reference is valid for as
// long as the `value` (or the cell behind it) lives — inside a tick, that is
// the tick. A type mismatch is a shell bug and throws, exactly like the scalar
// `std::get<T>` path.
//
// Declared before the generic `from_value` so its optional branch finds this
// overload by unqualified lookup at the point the generic body is defined.
/// Read a user struct from `v`, borrowing the payload: an inline value points
/// into the variant, a ref-counted one into its pool cell. The reference is
/// valid only while `v` (or its cell) lives — inside a tick, that is the tick.
/// A type mismatch throws `std::bad_variant_access`.
template<typename T>
requires kernel::user_value_c<T>
const T&
from_value(const kernel::value& v)
{
    if (const auto* in { std::get_if<kernel::user_inline>(&v) })
    {
        if (in->type == &kernel::tag_of<T>())
            return *reinterpret_cast<const T*>(in->bytes.data());
    }
    else if (const auto* ref { std::get_if<kernel::user_ref>(&v) })
    {
        if (ref->cell != nullptr && ref->cell->type == &kernel::tag_of<T>())
            return *static_cast<const T*>(kernel::user_payload(ref->cell));
    }

    throw std::bad_variant_access {};
}

/// Read a typed value from `v`. Scalars use the exact variant alternative
/// (`std::get<T>`); `std::optional<T>` reads `monostate` as disengaged and
/// otherwise recurses into its payload.
template<typename T>
constexpr
T
from_value(const kernel::value& v)
{
    if constexpr (is_optional_v<T>)
    {
        using payload_t = typename T::value_type;

        static_assert(!is_optional_v<payload_t>, "nested optionals are not port types - flatten them");
        static_assert(!std::same_as<payload_t, std::monostate>, "optional<monostate> is not a port type - use monostate");

        if (std::holds_alternative<std::monostate>(v))
            return std::nullopt;

        return from_value<payload_t>(v);
    }
    else
    {
        return std::get<T>(v);
    }
}

static_assert(to_value(std::monostate{}) == kernel::value{}, "unit converts to the empty value");
static_assert(to_value(true) == kernel::value{true}, "bool converts to the bool alternative");
static_assert(to_value(std::int64_t{23}) == kernel::value{23}, "int64 converts to the int64 alternative");
static_assert(to_value(-23) == kernel::value{-23}, "integral literals widen to int64");
static_assert(to_value(23.23) == kernel::value{23.23}, "double converts to the double alternative");
static_assert(to_value(23.23f) == kernel::value{23.23f}, "floating literals widen to double"); // TODO Fix warning (should be an error in this repo!)
static_assert(to_value(23) == to_value(std::int64_t{23}), "two write forms of 23 agree");
static_assert(to_value(1.5) == to_value(1.5f), "two write forms of 1.5 agree");
static_assert(from_value<std::monostate>(to_value(std::monostate{})) == std::monostate{}, "unit round-trips");
static_assert(from_value<bool>(to_value(true)) == true, "bool round-trips");
static_assert(from_value<std::int64_t>(to_value(23)) == 23, "int64 round-trips");
static_assert(from_value<double>(to_value(23.23)) == 23.23, "double round-trips");
static_assert(to_value(std::optional<double>{2.5}) == kernel::value{2.5}, "engaged optional converts its payload");
static_assert(to_value(std::optional<double>{}) == kernel::value{}, "disengaged optional converts to the empty value");
static_assert(to_value(std::nullopt) == kernel::value{}, "nullopt converts to the empty value");
static_assert(from_value<std::optional<double>>(to_value(std::optional<double>{2.5})) == std::optional<double>{2.5}, "optional round-trips");
static_assert(!from_value<std::optional<double>>(to_value(std::monostate{})).has_value(), "empty value reads back as disengaged");
static_assert(from_value<std::optional<std::int64_t>>(to_value(7)) == std::optional<std::int64_t>{7}, "optional payloads widen like bare values");

// ---------------------------------------------------------------------------
// the tick functions bridging the typed shell and the erased kernel
//
// A recipe node lowers to a `node_descriptor::tick_fn` — a function pointer over
// a span of `kernel::value` — whose type knowledge is baked in here at
// composition time. The AST itself (a `std::variant` of node types, walked by
// `std::visit`) lives in ast.hpp; the shell's operator templates produce
// tick function pointers through the helpers below and store them in the AST.
//
// `first_anchor` (in ast.hpp) is the look-ahead that makes boundary anchoring
// work: the first input/output port encountered walking the fragment left to
// right. When a fragment is reached with no clock established yet, the walker
// seeds the current clock from that anchor before emitting anything, so
// interior nodes always know where they run.
// ---------------------------------------------------------------------------

namespace detail
{

// Bridge from a shell value to a kernel value. Scalars and optionals keep the
// constexpr `to_value`; user structs are constructed from the region pool the
// tick context carries — inline when small, in a ref-counted cell otherwise.
template<typename T>
constexpr
kernel::value
to_kernel_value(const kernel::tick_context& ctx, T&& v)
{
    using U = std::remove_cvref_t<T>;

    if constexpr (is_optional_v<U>)
    {
        if (!v)
            return kernel::value { std::monostate {} };

        return to_kernel_value(ctx, *v);
    }
    else if constexpr (kernel::user_value_c<U>)
    {
        return kernel::make_user_value<U>(ctx.home, std::forward<T>(v));
    }
    else
    {
        return to_value(std::forward<T>(v));
    }
}

// The seed storage a delay/scan recipe owns: a `kernel::value` for scalars
// (`to_value`, as before), an `closure_cell`-held T for user structs (the delay
// tick constructs a kernel value from it on the first tick). Lives here, not in
// ast.hpp, so the scalar branch can call `to_value` by ordinary lookup.
template<typename T>
constexpr
std::variant<kernel::value, detail::closure_cell>
make_init_storage(T init)
{
    if constexpr (kernel::user_value_c<T>)
        return detail::closure_cell { std::move(init) };
    else
        return to_value(std::move(init));
}

// Stateless: F is reconstructed as a type — no captures, no self. This is the
// one shape that can become constant data (chunk F/G constexpr recipe objects).
template<typename fn_t, typename in_t, typename out_t>
constexpr kernel::value
stateless_tick(const void*, const kernel::tick_context& ctx, std::span<const kernel::value> in)
{
    return to_kernel_value(ctx, std::invoke(fn_t{}, from_value<in_t>(in.front())));
}

template<typename fn_t, typename in1_t, typename in2_t, typename out_t>
constexpr kernel::value
stateless_tick2(const void*, const kernel::tick_context& ctx, std::span<const kernel::value> in)
{
    return to_kernel_value(
        ctx,
        std::invoke(
            fn_t{},
            from_value<in1_t>(in[0]),
            from_value<in2_t>(in[1])
        )
    );
}

// Stateful: fn_t's captures live inside the recipe node; `self` points at the
// closure object directly. The pointer is constant-representable (it points
// into a constexpr recipe), but the tick body still reads through `self` at
// runtime.
template<typename fn_t, typename in_t, typename out_t>
kernel::value
boxed_tick(const void* self, const kernel::tick_context& ctx, std::span<const kernel::value> in)
{
    const auto& fn { *static_cast<const fn_t*>(self) };
    return to_kernel_value(ctx, std::invoke(fn, from_value<in_t>(in.front())));
}

template<typename fn_t, typename in1_t, typename in2_t, typename out_t>
kernel::value
boxed_tick2(const void* self, const kernel::tick_context& ctx, std::span<const kernel::value> in)
{
    const auto& fn { *static_cast<const fn_t*>(self) };

    return to_kernel_value(
        ctx,
        std::invoke(
            fn,
            from_value<in1_t>(in[0]),
            from_value<in2_t>(in[1])
        )
    );
}

// N-ary: one fan-in edge per leaf of the (flattened) input tuple, converted
// per edge in zip order — the same contract as tick2, generalized from 2 to
// N via an index sequence. Stateless/boxed split mirrors tick/tick2.
template<typename fn_t, typename tuple_ins_t, typename out_t, std::size_t... indices>
constexpr kernel::value
stateless_tick_n_impl(const void*, const kernel::tick_context& ctx, std::span<const kernel::value> in, std::index_sequence<indices...>)
{
    return to_kernel_value(
        ctx,
        std::invoke(
            fn_t{},
            from_value<std::tuple_element_t<indices, tuple_ins_t>>(in[indices])...
        )
    );
}

template<typename fn_t, typename tuple_ins_t, typename out_t>
constexpr kernel::value
stateless_tick_n(const void* self, const kernel::tick_context& ctx, std::span<const kernel::value> in)
{
    return stateless_tick_n_impl<fn_t, tuple_ins_t, out_t>(
        self, ctx, in, std::make_index_sequence<std::tuple_size_v<tuple_ins_t>>{});
}

template<typename fn_t, typename tuple_ins_t, typename out_t, std::size_t... indices>
kernel::value
boxed_tick_n_impl(const void* self, const kernel::tick_context& ctx, std::span<const kernel::value> in, std::index_sequence<indices...>)
{
    const auto& fn { *static_cast<const fn_t*>(self) };

    return to_kernel_value(
        ctx,
        std::invoke(
            fn,
            from_value<std::tuple_element_t<indices, tuple_ins_t>>(in[indices])...
        )
    );
}

template<typename fn_t, typename tuple_ins_t, typename out_t>
kernel::value
boxed_tick_n(const void* self, const kernel::tick_context& ctx, std::span<const kernel::value> in)
{
    return boxed_tick_n_impl<fn_t, tuple_ins_t, out_t>(
        self, ctx, in, std::make_index_sequence<std::tuple_size_v<tuple_ins_t>>{});
}

} // namespace detail

// ---------------------------------------------------------------------------
// recipe<in_t, out_t> — the typed shell
// ---------------------------------------------------------------------------

/// A typed, immutable description of a signal network.
///
/// `recipe<in_t, out_t>` is a by-value arena of nodes plus its root index: it
/// owns no runtime state and allocates nothing, so it can live in a constant
/// expression. Operators build larger recipes from smaller ones. Lower it
/// into a `kernel::program` (owning), or into a constant
/// `shell::descriptor_storage` with `shell::compile` (borrowing).
template<typename in_t, typename out_t, typename skeleton_t = skeleton::tag_list<>, std::size_t NodeCount = 0>
class recipe
{
public:
    /// The compile-time skeleton of the fragment: the multiset of
    /// graph-emitting operators, used to size a compiled program exactly.
    /// Callers never name it; operators stamp it by template deduction.
    using ast = skeleton_t;

    /// Number of arena nodes (structural + graph-emitting). The arena is sized
    /// from it at compile time, so a recipe is constant-initializable with no
    /// heap allocation.
    static constexpr std::size_t node_count { NodeCount };

    /// Build from a by-value arena plus the index of the root node.
    constexpr
    recipe(std::array<node, node_count> arena, node_ref root) :
        m_arena { std::move(arena) },
        m_root { root }
    {}

    /// The root recipe node, the entry point for the lowering walk.
    constexpr const node&
    root() const
    {
        return m_arena[m_root];
    }

    /// The whole arena by value. Walkers thread this view through child
    /// resolution.
    constexpr std::span<const node>
    arena() const
    {
        return m_arena;
    }

    /// The arena as a fixed-size array, retained for concatenation when
    /// composing networks.
    constexpr const std::array<node, node_count>&
    storage() const noexcept
    {
        return m_arena;
    }

    /// Index of the root node inside `storage()`.
    constexpr node_ref
    root_index() const noexcept
    {
        return m_root;
    }

private:
    std::array<node, node_count> m_arena;
    node_ref m_root;
};

/// True when `T` is a `recipe`.
template<typename T>
struct is_recipe : std::false_type {};

template<typename in_t, typename out_t, typename skeleton_t, std::size_t N>
struct is_recipe<recipe<in_t, out_t, skeleton_t, N>> : std::true_type {};

/// Any `recipe`, regardless of its endpoints or skeleton.
template<typename T>
concept recipe_c = is_recipe<std::remove_cvref_t<T>>::value;

// ---------------------------------------------------------------------------
// operators
// ---------------------------------------------------------------------------

namespace detail
{

// The ast walker (defined in ast.hpp): dispatches instantiate over the tagged
// node. Declared here so the recipe-entry below can call it with only a
// forward-declared node.
constexpr
std::vector<kernel::node_id>
instantiate(const node&, std::span<const node> arena, draft&, std::optional<kernel::clock_tag>&, std::span<const kernel::node_id>);

} // namespace detail

/// Emit a whole network into `draft`. The empty input span matches the
/// `std::monostate` convention of source networks. `current` is the walker's
/// clock state: pass `std::nullopt` at the root — every branch seeds itself
/// from its own anchors. Returns the ids of the emitted output nodes.
template<typename in_t, typename out_t, typename skeleton_t, std::size_t N>
constexpr
std::vector<kernel::node_id>
instantiate(draft& draft, std::optional<kernel::clock_tag>& current, const recipe<in_t, out_t, skeleton_t, N>& network)
{
    const auto result { detail::instantiate(network.root(), network.arena(), draft, current, {}) };

    // Guard the skeleton against divergence from the walker. The skeleton is a
    // tight upper bound, not an exact count: `share` deduplicates at the value
    // level (pointer identity), which no type can see, so a shared fragment used
    // N times contributes N tags but emits once. Exactness holds for share-free
    // networks; the assert enforces that the walk never EXCEEDS the budget.
    assert(draft.node_count() <= skeleton::node_count<skeleton_t>);
    assert(draft.edge_count() <= skeleton::edge_count<skeleton_t>);

    return result;
}

} // namespace clockr::shell

// The operator functions live in recipe/operator/*.hpp (one file per operator),
// included here — after recipe<in,out> is defined and the namespace is closed — so
// <recipe/recipe.hpp> remains the single umbrella header callers include.
#include "clockr/shell/valid.hpp"
#include "clockr/shell/operator/at.hpp"
#include "clockr/shell/operator/lift.hpp"
#include "clockr/shell/operator/compose.hpp"
#include "clockr/shell/operator/input.hpp"
#include "clockr/shell/operator/output.hpp"
#include "clockr/shell/operator/share.hpp"
#include "clockr/shell/operator/scan.hpp"
#include "clockr/shell/operator/delay.hpp"
#include "clockr/shell/operator/stepper.hpp"
#include "clockr/shell/operator/zip.hpp"
#include "clockr/shell/operator/parallel.hpp"
