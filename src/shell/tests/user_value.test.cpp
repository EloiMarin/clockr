#include "clockr/shell/recipe.hpp"
#include "clockr/shell/builder.hpp"
#include "clockr/shell/descriptor_storage.hpp"

#include "clockr/kernel/release_engine.hpp"

#include "clockr/kernel/value.hpp"

#include <catch2/catch_test_macros.hpp>

#include "clockr/mem/pool.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

using namespace clockr;

namespace
{
    // Small and trivially copyable: stored inline.
    struct point
    {
        int x { 0 };
        int y { 0 };
    };

    // A distinct inline type, for the strict-mismatch check.
    struct vec2
    {
        int x { 0 };
        int y { 0 };
    };

    // Too large for the inline budget: ref-counted.
    struct triple
    {
        std::int64_t a { 0 };
        std::int64_t b { 0 };
        std::int64_t c { 0 };
    };

    // Inline, with a described debug schema.
    struct flagged
    {
        double level { 0.0 };
        bool mute { false };
    };
} // namespace

namespace clockr::kernel
{
template<>
struct describe<flagged>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("level", &flagged::level),
            field("mute", &flagged::mute),
        };
        static const user_type t { make_user_type<flagged>("test.flagged", fields) };

        return t;
    }
};
} // namespace clockr::kernel


TEST_CASE("shell/user_value | Small structs convert inline", "[recipe][user_value]")
{
    const kernel::tick_context ctx {}; // no pool: inline types ignore it

    const auto v { shell::detail::to_kernel_value(ctx, point { 1, 2 }) };

    REQUIRE(std::holds_alternative<kernel::user_inline>(v));
    REQUIRE(shell::from_value<point>(v).x == 1);
    REQUIRE(shell::from_value<point>(v).y == 2);
}

TEST_CASE("shell/user_value | Large structs route through the region pool", "[recipe][user_value]")
{
    kernel::value_home home { 64 * 1024 };
    const kernel::tick_context ctx { &home };

    const auto baseline { home.pool.snapshot().used_bytes };

    const auto v { shell::detail::to_kernel_value(ctx, triple { 1, 2, 3 }) };

    REQUIRE(std::holds_alternative<kernel::user_ref>(v));
    REQUIRE(home.pool.snapshot().used_bytes > baseline);
    REQUIRE(shell::from_value<triple>(v).b == 2);

    kernel::release(v);
    REQUIRE(home.pool.snapshot().used_bytes > baseline); // retired, not yet reclaimed

    while (kernel::user_cell* cell { home.retired.pop() })
        kernel::reclaim(cell, &home.pool);

    REQUIRE(home.pool.snapshot().used_bytes == baseline);
}

TEST_CASE("shell/user_value | Reading a user struct is strict about its type", "[recipe][user_value]")
{
    const auto v { shell::detail::to_kernel_value(kernel::tick_context {}, point { 1, 2 }) };

    REQUIRE(shell::from_value<point>(v).x == 1);
    REQUIRE_THROWS_AS(shell::from_value<vec2>(v), std::bad_variant_access);
    REQUIRE_THROWS_AS(shell::from_value<std::int64_t>(v), std::bad_variant_access);
    REQUIRE_THROWS_AS(shell::from_value<point>(kernel::value { std::int64_t { 1 } }), std::bad_variant_access);
}

TEST_CASE("shell/user_value | Optional user values convert like other optionals", "[recipe][user_value]")
{
    const auto engaged { shell::detail::to_kernel_value(
        kernel::tick_context {}, std::optional<point> { point { 3, 4 } }) };

    REQUIRE(std::holds_alternative<kernel::user_inline>(engaged));
    REQUIRE(shell::from_value<std::optional<point>>(engaged).value_or(point {}).x == 3);

    const auto disengaged { shell::detail::to_kernel_value(
        kernel::tick_context {}, std::optional<point> {}) };

    REQUIRE(std::holds_alternative<std::monostate>(disengaged));
    REQUIRE_FALSE(shell::from_value<std::optional<point>>(disengaged).has_value());
}

TEST_CASE("shell/user_value | A user-valued lift round-trips through the engine", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<point, kernel::clock_tag::ui>("p")
        >> shell::lift<point, std::int64_t>([](const point& q) { return q.x + q.y; })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("sum")
    };

    kernel::program program { network };
    kernel::release_engine engine;

    const auto* const home { program.region(kernel::clock_tag::ui).home };

    program.set("p", kernel::make_user_value<point>(home, 3, 4));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "sum") == 7);
}

TEST_CASE("shell/user_value | An optional user value reaches a scalar output", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<std::optional<point>, kernel::clock_tag::ui>("p")
        >> shell::lift<std::optional<point>, std::int64_t>(
            [](const std::optional<point>& q) { return q ? q->x + q->y : -1; })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("sum")
    };

    kernel::program program { network };
    kernel::release_engine engine;

    const auto* const home { program.region(kernel::clock_tag::ui).home };

    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "sum") == -1);

    program.set("p", kernel::make_user_value<point>(home, 5, 6));
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "sum") == 11);
}

TEST_CASE("shell/user_value | scan accumulates a user struct across ticks", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::ui>("n")
        >> shell::scan<std::int64_t, point>(
            point { 0, 0 },
            [](const point& state, const std::int64_t& in)
            {
                return point { state.x + static_cast<int>(in), state.y + 1 };
            })
        >> shell::output<point, kernel::clock_tag::ui>("acc")
    };

    kernel::program program { network };
    kernel::release_engine engine;

    program.set("n", std::int64_t { 5 });
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<point>(program, "acc").x == 5);
    REQUIRE(get<point>(program, "acc").y == 1);

    program.set("n", std::int64_t { 3 });
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<point>(program, "acc").x == 8);
    REQUIRE(get<point>(program, "acc").y == 2);
}

TEST_CASE("shell/user_value | delay seeds a user struct on the first tick", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<point, kernel::clock_tag::ui>("p")
        >> shell::delay<point>(point { 7, 8 })
        >> shell::output<point, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };
    kernel::release_engine engine;

    program.set<point>("p", 1, 2);
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<point>(program, "out").x == 7);
    REQUIRE(get<point>(program, "out").y == 8);

    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<point>(program, "out").x == 1);
    REQUIRE(get<point>(program, "out").y == 2);
}

TEST_CASE("shell/user_value | set<T> and get<T> carry a pooled struct", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<triple, kernel::clock_tag::ui>("t")
        >> shell::lift<triple, std::int64_t>(
            [](const triple& t) { return t.a * 100 + t.b * 10 + t.c; })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("sum")
    };

    kernel::program program { network };
    kernel::release_engine engine;

    program.set<triple>("t", 1, 2, 3);
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<std::int64_t>(program, "sum") == 123);
}

TEST_CASE("shell/user_value | The registry carries described type schemas", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<flagged, kernel::clock_tag::ui>("f")
        >> shell::lift<flagged, double>([](const flagged& f) { return f.level; })
        >> shell::output<double, kernel::clock_tag::ui>("level")
    };

    kernel::program program { network };

    REQUIRE(program.types().size() == 1);
    REQUIRE(program.types()[0] == &kernel::tag_of<flagged>());
    REQUIRE(program.types()[0]->name != nullptr);
    REQUIRE(std::string_view { program.types()[0]->name } == "test.flagged");
    REQUIRE(program.types()[0]->fields.size() == 2);

    // The registry rides the compiled descriptor too, so a borrowing program
    // carries the same schema.
    const auto net { shell::compile(network) };
    kernel::program borrowed { net.descriptor() };

    REQUIRE(borrowed.types().size() == 1);
    REQUIRE(borrowed.types()[0] == &kernel::tag_of<flagged>());
}

TEST_CASE("shell/user_value | Anonymous types send no schema", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<point, kernel::clock_tag::ui>("p")
        >> shell::lift<point, std::int64_t>([](const point& q) { return q.x + q.y; })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("sum")
    };

    kernel::program program { network };

    REQUIRE(program.types().empty());
}

TEST_CASE("shell/user_value | Region pool size is configurable", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<point, kernel::clock_tag::ui>("p")
        >> shell::output<point, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network, kernel::program_options { .region_pool_bytes = 16 * 1024 } };

    REQUIRE(program.region(kernel::clock_tag::ui).home->pool.capacity() == 16 * 1024);
}

TEST_CASE("shell/user_value | A compiled descriptor materializes its user seed too", "[recipe][user_value]")
{
    const auto recipe
    {
        shell::input<triple, kernel::clock_tag::ui>("t")
        >> shell::delay<triple>(triple { 1, 2, 3 })
        >> shell::output<triple, kernel::clock_tag::ui>("out")
    };

    const auto net { shell::compile(recipe) };
    kernel::program borrowed { net.descriptor() };

    // The borrowing path materializes into its own region pool at
    // construction, exactly like the owning path.
    REQUIRE(borrowed.region(kernel::clock_tag::ui).home->pool.snapshot().used_bytes > 0);

    borrowed.set<triple>("t", 4, 5, 6);

    kernel::release_engine engine;
    engine.step(borrowed, kernel::clock_tag::ui);
    REQUIRE(get<triple>(borrowed, "out").a == 1);
    REQUIRE(get<triple>(borrowed, "out").c == 3);
}

TEST_CASE("shell/user_value | The dev build path materializes a user seed", "[recipe][user_value]")
{
    const auto network
    {
        shell::input<triple, kernel::clock_tag::ui>("t")
        >> shell::delay<triple>(triple { 1, 2, 3 })
        >> shell::output<triple, kernel::clock_tag::ui>("out")
    };

    // `shell::build` has no region pools: it materializes on the heap. The point
    // of this test is that it builds at all (a user seed has no cell record),
    // and that the delay node produces the seed.
    const auto dev_graph { shell::build(network) };

    // `size`/`nodes`/`kind`/`evaluate` are hidden friends of the dev node and
    // graph; call them unqualified so ADL finds them.
    REQUIRE(size(dev_graph) == 3);

    const kernel::node* delay_node { nullptr };

    for (const auto& node : nodes(dev_graph))
        if (kind(node) == kernel::node_kind::delay)
            delay_node = &node;

    REQUIRE(delay_node != nullptr);

    const auto seed { tick(*delay_node, kernel::tick_context {}, {}) };
    REQUIRE(std::holds_alternative<kernel::user_ref>(seed));
    REQUIRE(shell::from_value<triple>(seed).b == 2);

    kernel::release(seed);
}

TEST_CASE("shell/user_value | Owning and compiled (borrowing) programs agree", "[recipe][user_value]")
{
    const auto recipe
    {
        shell::input<point, kernel::clock_tag::ui>("p")
        >> shell::lift<point, std::int64_t>([](const point& q) { return q.x * 100 + q.y; })
        >> shell::output<std::int64_t, kernel::clock_tag::ui>("v")
    };

    kernel::program owned { recipe };

    const auto net { shell::compile(recipe) };
    kernel::program borrowed { net.descriptor() };

    kernel::release_engine e1;
    kernel::release_engine e2;

    owned.set<point>("p", 3, 4);
    borrowed.set<point>("p", 3, 4);

    e1.step(owned, kernel::clock_tag::ui);
    e2.step(borrowed, kernel::clock_tag::ui);

    REQUIRE(get<std::int64_t>(owned, "v") == 304);
    REQUIRE(get<std::int64_t>(borrowed, "v") == 304);
    REQUIRE(borrowed.types().empty()); // `point` has no describe
}
