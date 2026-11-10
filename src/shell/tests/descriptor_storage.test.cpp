#include "clockr/shell/descriptor_storage.hpp"

#include "clockr/kernel/node_descriptor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace
{

using namespace clockr;

constexpr auto passthrough_tick
{
    [](const void*, const kernel::tick_context&, std::span<const kernel::value> inputs)
    {
        return inputs.empty()
            ? kernel::value { std::monostate {} }
            : inputs.front();
    }
};

// --- the skeleton counts themselves (the source of truth for sizing) ---

using passthrough_ast =
    shell::skeleton::tag_list<shell::skeleton::input_t, shell::skeleton::lift_t, shell::skeleton::output_t>;

static_assert(shell::skeleton::node_count<passthrough_ast> == 3);
static_assert(shell::skeleton::edge_count<passthrough_ast> == 2);
static_assert(shell::skeleton::input_count<passthrough_ast> == 1);
static_assert(shell::skeleton::output_count<passthrough_ast> == 1);
static_assert(shell::skeleton::scan_count<passthrough_ast> == 0);

using scan_ast = shell::skeleton::tag_list<shell::skeleton::input_t, shell::skeleton::scan_t>;
static_assert(shell::skeleton::node_count<scan_ast> == 3);   // input + (delay + scanner)
static_assert(shell::skeleton::edge_count<scan_ast> == 3);   // input 0 + scan 3

// --- the operators stamp the skeleton, visible through decltype (unevaluated) ---

constexpr auto stamped = [] {
    using net = decltype(
        shell::input<std::int64_t, kernel::clock_tag::audio>("in")
        >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; })
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    );
    return shell::skeleton::node_count<typename net::ast>;
}();
static_assert(stamped == 3);

// --- a fabricated passthrough baked into a constexpr descriptor_storage: the exact,
//     no-margin "compiled program is data" milestone, hand-built before chunk G
//     wires the walk ---

constexpr shell::descriptor_storage<passthrough_ast>
make_passthrough()
{
    shell::descriptor_storage<passthrough_ast> net;

    net.node_count = 3;
    net.edge_count = 2;
    net.input_count = 1;
    net.output_count = 1;

    net.nodes[0] = { 0, kernel::clock_tag::audio, kernel::node_kind::input,    passthrough_tick, nullptr };
    net.nodes[1] = { 1, kernel::clock_tag::audio, kernel::node_kind::internal, passthrough_tick, nullptr };
    net.nodes[2] = { 2, kernel::clock_tag::audio, kernel::node_kind::output,   passthrough_tick, nullptr };

    net.edges[0] = { 0, 1 };
    net.edges[1] = { 1, 2 };

    net.fan_in[0] = 0;
    net.fan_in[1] = 1;
    net.fan_in_offsets[0] = 0;
    net.fan_in_offsets[1] = 0;
    net.fan_in_offsets[2] = 1;
    net.fan_in_offsets[3] = 2;

    net.evaluation_order[0] = 0;
    net.evaluation_order[1] = 1;
    net.evaluation_order[2] = 2;

    net.input_ids[0] = 0;
    net.output_ids[0] = 2;

    net.input_names[0] = "in";
    net.output_names[0] = "out";

    return net;
}

constexpr auto net { make_passthrough() };

static_assert(sizeof(net.nodes) == 3 * sizeof(kernel::node_descriptor));   // EXACT — no margin
static_assert(net.descriptor().nodes.size() == 3);
static_assert(net.descriptor().edges.size() == 2);
static_assert(net.descriptor().input_ids.size() == 1);
static_assert(net.descriptor().output_ids.size() == 1);
static_assert(std::string_view { net.descriptor().input_names[0] } == "in");
static_assert(net.descriptor().evaluation_order[2] == 2);
static_assert(net.descriptor().fan_in_offsets[3] == 2);

// --- scale: a 512-node list is one flat fold, no template-depth stress ---

template<typename T, std::size_t>
using repeat_tag = T;

template<typename T, typename Seq>
struct repeat;

template<typename T, std::size_t... I>
struct repeat<T, std::index_sequence<I...>>
{
    using type = shell::skeleton::tag_list<repeat_tag<T, I>...>;
};

using big_ast = repeat<shell::skeleton::lift_t, std::make_index_sequence<512>>::type;
static_assert(shell::skeleton::node_count<big_ast> == 512);
static_assert(shell::skeleton::edge_count<big_ast> == 512);

// --- the canonical constexpr path: `shell::compile` over a stateless network ---
//
// input >> lift >> output, with a captureless lift, is a fully constant
// expression: the recipe, the walk, and the compiled descriptor are all decided
// at compile time. Only stateless closures qualify —
// stateful/boxed ticks read their state through `self`, a C++26-only constant
// expression (rule 7), so they are runtime-only.

constexpr auto compiled_net
{
    shell::input<std::int64_t, kernel::clock_tag::audio>("in")
    >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 1; })
    >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
};

// MSVC's constexpr evaluator cannot fold the vector-based walk that
// `shell::compile` performs: a std::vector returned through the std::visit node
// dispatcher reads back as uninitialized (C2131). The walker itself is fine, so
// on MSVC `compile` is an ordinary runtime call and the same properties are
// asserted at runtime in the MSVC-only test case below. Other compilers keep the
// full compile-time guarantee.
#if defined(_MSC_VER)
const auto compiled { shell::compile(compiled_net) };
#else
constexpr auto compiled { shell::compile(compiled_net) };

static_assert(compiled.node_count == 3);
static_assert(compiled.edge_count == 2);
static_assert(compiled.input_count == 1);
static_assert(compiled.output_count == 1);
static_assert(compiled.descriptor().nodes.size() == 3);
static_assert(compiled.descriptor().edges.size() == 2);
static_assert(compiled.descriptor().input_ids.size() == 1);
static_assert(compiled.descriptor().output_ids.size() == 1);
static_assert(std::string_view { compiled.descriptor().input_names[0] } == "in");
static_assert(std::string_view { compiled.descriptor().output_names[0] } == "out");
static_assert(compiled.descriptor().evaluation_order.size() == 3);
#endif

// --- `share` is constexpr too: the whole operator set is compile-able ----
//
// `share` mints its identity from the call site's std::source_location (a
// compile-time constant), not a runtime counter, so a shared network is a
// constant expression as well. Here the diamond reuses one shared source and
// must dedup it to a single input node.

constexpr auto shared_source { shell::share(shell::input<std::int64_t, kernel::clock_tag::audio>("in")) };

constexpr auto shared_net
{
    shell::zip(
        shared_source >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x * 2; }),
        shared_source >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& x) { return x + 10; })
    )
    >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
           [](const std::int64_t& a, const std::int64_t& b) { return a + b; }
       )
    >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
};

#if defined(_MSC_VER)
const auto shared_compiled { shell::compile(shared_net) };
#else
constexpr auto shared_compiled { shell::compile(shared_net) };

static_assert(shared_compiled.input_count == 1);   // the share deduped the source
static_assert(shared_compiled.output_count == 1);
#endif

} // namespace

TEST_CASE("shell/compile | A descriptor_storage is deeply constant-initializable with exact sizing", "[constexpr]")
{
    // The static_asserts above prove constness and exactness; here we confirm
    // the derived graph is a faithful view the engine-facing accessors agree with.
    using namespace clockr;

    REQUIRE(kernel::size(net.descriptor()) == 3);
    REQUIRE(kernel::kind(kernel::at(net.descriptor(), 0)) == kernel::node_kind::input);
    REQUIRE(kernel::kind(kernel::at(net.descriptor(), 2)) == kernel::node_kind::output);

    const auto fan_in { kernel::fan_in(net.descriptor(), 1) };
    REQUIRE(fan_in.size() == 1);
    REQUIRE(fan_in[0] == 0);
}

#if defined(_MSC_VER)
TEST_CASE("shell/compile | The canonical paths fold at runtime under MSVC", "[constexpr]")
{
    // Runtime mirror of the static_asserts that the other compilers evaluate at
    // compile time (MSVC cannot constant-evaluate the vector-based walk).
    using namespace clockr;

    REQUIRE(compiled.node_count == 3);
    REQUIRE(compiled.edge_count == 2);
    REQUIRE(compiled.input_count == 1);
    REQUIRE(compiled.output_count == 1);
    REQUIRE(compiled.descriptor().nodes.size() == 3);
    REQUIRE(compiled.descriptor().edges.size() == 2);
    REQUIRE(compiled.descriptor().input_ids.size() == 1);
    REQUIRE(compiled.descriptor().output_ids.size() == 1);
    REQUIRE(std::string_view { compiled.descriptor().input_names[0] } == "in");
    REQUIRE(std::string_view { compiled.descriptor().output_names[0] } == "out");
    REQUIRE(compiled.descriptor().evaluation_order.size() == 3);

    REQUIRE(shared_compiled.input_count == 1);   // the share deduped the source
    REQUIRE(shared_compiled.output_count == 1);
}
#endif
