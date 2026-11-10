#include "clockr/kernel/user_type.hpp"
#include "clockr/kernel/value.hpp"

#include <catch2/catch_test_macros.hpp>

#include "clockr/mem/pool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

namespace clockr::kernel
{
namespace user_value_test
{

// Stored inline: small and trivially copyable.
struct point
{
    int x { 0 };
    int y { 0 };
};

// Too large for the inline budget: takes the ref-counted path.
struct triple
{
    std::int64_t a { 0 };
    std::int64_t b { 0 };
    std::int64_t c { 0 };
};

// Non-trivially-copyable: takes the ref-counted path even though it is small.
struct owning
{
    int* destroyed { nullptr };

    explicit owning(int* counter) : destroyed { counter } {}

    ~owning()
    {
        if (destroyed != nullptr)
            ++(*destroyed);
    }
};

struct inner
{
    double q { 0.0 };
};

struct outer
{
    float a { 0.0f };
    inner nested {};
    std::int64_t n { 0 };
    bool flag { false };
};

// Optional members of every payload kind and width, so the engaged/absent
// read contract is pinned everywhere the debugger relies on it.
struct maybe
{
    std::optional<bool> opt_flag {};
    std::optional<std::int8_t> opt_i8 {};
    std::optional<std::int16_t> opt_i16 {};
    std::optional<std::int32_t> opt_i32 {};
    std::optional<std::int64_t> opt_i64 {};
    std::optional<std::uint8_t> opt_u8 {};
    std::optional<std::uint16_t> opt_u16 {};
    std::optional<std::uint32_t> opt_u32 {};
    std::optional<std::uint64_t> opt_u64 {};
    std::optional<float> opt_f32 {};
    std::optional<double> opt_f64 {};
};

enum class phase : std::uint8_t
{
    idle,
    active,
    done,
};

// A text member and an enum member: the two kinds that have no direct
// kernel::value representation and are rendered through the schema.
struct texty
{
    std::string label {};
    phase state { phase::idle };
    std::int64_t count { 0 };
};

// One member of every describable scalar kind and width, so the schema's
// kind/size/offset contract is pinned where the debugger's readers rely on it.
struct kinds
{
    bool flag { false };
    std::int8_t i8 { 0 };
    std::int16_t i16 { 0 };
    std::int32_t i32 { 0 };
    std::int64_t i64 { 0 };
    std::uint8_t u8 { 0 };
    std::uint16_t u16 { 0 };
    std::uint32_t u32 { 0 };
    std::uint64_t u64 { 0 };
    float f32 { 0.0f };
    double f64 { 0.0 };
    inner nested {};
};

} // namespace user_value_test

template<>
struct describe<user_value_test::inner>
{
    static const user_type& type()
    {
        static const std::array fields { field("q", &user_value_test::inner::q) };
        static const user_type t { make_user_type<user_value_test::inner>("test.inner", fields) };

        return t;
    }
};

template<>
struct describe<user_value_test::outer>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("a", &user_value_test::outer::a),
            field("nested", &user_value_test::outer::nested),
            field("n", &user_value_test::outer::n),
            field("flag", &user_value_test::outer::flag),
        };
        static const user_type t { make_user_type<user_value_test::outer>("test.outer", fields) };

        return t;
    }
};

template<>
struct describe<user_value_test::kinds>
{
    static const user_type& type()
    {
        using user_value_test::kinds;

        static const std::array fields
        {
            field("flag", &kinds::flag),
            field("i8", &kinds::i8),
            field("i16", &kinds::i16),
            field("i32", &kinds::i32),
            field("i64", &kinds::i64),
            field("u8", &kinds::u8),
            field("u16", &kinds::u16),
            field("u32", &kinds::u32),
            field("u64", &kinds::u64),
            field("f32", &kinds::f32),
            field("f64", &kinds::f64),
            field("nested", &kinds::nested),
        };
        static const user_type t { make_user_type<kinds>("test.kinds", fields) };

        return t;
    }
};

template<>
struct describe<user_value_test::maybe>
{
    static const user_type& type()
    {
        using user_value_test::maybe;

        static const std::array fields
        {
            field("opt_flag", &maybe::opt_flag),
            field("opt_i8", &maybe::opt_i8),
            field("opt_i16", &maybe::opt_i16),
            field("opt_i32", &maybe::opt_i32),
            field("opt_i64", &maybe::opt_i64),
            field("opt_u8", &maybe::opt_u8),
            field("opt_u16", &maybe::opt_u16),
            field("opt_u32", &maybe::opt_u32),
            field("opt_u64", &maybe::opt_u64),
            field("opt_f32", &maybe::opt_f32),
            field("opt_f64", &maybe::opt_f64),
        };
        static const user_type t { make_user_type<maybe>("test.maybe", fields) };

        return t;
    }
};

template<>
struct describe<user_value_test::texty>
{
    static const user_type& type()
    {
        using user_value_test::phase;
        using user_value_test::texty;

        static const std::array phases
        {
            enum_entry { 0, "idle" },
            enum_entry { 1, "active" },
            enum_entry { 2, "done" },
        };

        static const std::array fields
        {
            field("label", &texty::label),
            enum_field("state", &texty::state, phases),
            field("count", &texty::count),
        };
        static const user_type t { make_user_type<texty>("test.texty", fields) };

        return t;
    }
};

} // namespace clockr::kernel

namespace
{
using namespace clockr::kernel;
using namespace clockr::kernel::user_value_test;

namespace mem = clockr::mem;
} // namespace

TEST_CASE("kernel/value | The value variant stays trivially copyable and 32 bytes", "[kernel]")
{
    static_assert(std::is_trivially_copyable_v<value>);
    static_assert(std::is_trivially_destructible_v<value>);
    static_assert(sizeof(value) == 32);
    static_assert(sizeof(user_inline) == 24);
    static_assert(alignof(user_inline) == 8);

    static_assert(is_inlineable_user_v<point>, "small trivially-copyable PODs are inline");
    static_assert(!is_inlineable_user_v<triple>, "structs above the inline budget use a cell");
    static_assert(!is_inlineable_user_v<owning>, "non-trivially-copyable structs use a cell");

    static_assert(user_value_c<point> && user_value_c<outer>);
    static_assert(!user_value_c<std::int64_t>);
    static_assert(!user_value_c<bool>);
    static_assert(!user_value_c<std::monostate>);
}

TEST_CASE("kernel/value | make_user_value dispatches by size and triviality", "[kernel]")
{
    const auto small { make_user_value<point>(nullptr, 1, 2) };
    REQUIRE(std::holds_alternative<user_inline>(small));
    REQUIRE_FALSE(std::holds_alternative<user_ref>(small));
    REQUIRE(type_of(small) == &tag_of<point>());

    const auto large { make_user_value<triple>(nullptr, 1, 2, 3) };
    REQUIRE(std::holds_alternative<user_ref>(large));
    REQUIRE(type_of(large) == &tag_of<triple>());

    const auto owned { make_user_value<owning>(nullptr, nullptr) };
    REQUIRE(std::holds_alternative<user_ref>(owned));

    release(large);
    release(owned);
}

TEST_CASE("kernel/value | Anonymous types get a stable id-0 tag", "[kernel]")
{
    const auto& tag { tag_of<point>() };

    REQUIRE(tag.id == 0u);
    REQUIRE(tag.name == nullptr);
    REQUIRE(tag.size == sizeof(point));
    REQUIRE(tag.fields.empty());
    REQUIRE(&tag_of<point>() == &tag);

    static_assert(!described_c<point>);
    static_assert(described_c<outer>);
}

TEST_CASE("kernel/value | Described types expose a schema", "[kernel]")
{
    const auto& tag { tag_of<outer>() };

    REQUIRE(tag.name != nullptr);
    REQUIRE(std::string_view{ tag.name } == "test.outer");
    REQUIRE(tag.id != 0u);
    REQUIRE(tag.size == sizeof(outer));
    REQUIRE(tag.fields.size() == 4);

    REQUIRE(std::string_view{ tag.fields[0].name } == "a");
    REQUIRE(tag.fields[0].kind == field_kind::floating);
    REQUIRE(tag.fields[0].offset == offsetof(outer, a));

    REQUIRE(std::string_view{ tag.fields[1].name } == "nested");
    REQUIRE(tag.fields[1].kind == field_kind::nested);
    REQUIRE(tag.fields[1].nested == &tag_of<inner>());
    REQUIRE(tag.fields[1].offset == offsetof(outer, nested));

    REQUIRE(tag.fields[2].kind == field_kind::integer);
    REQUIRE(tag.fields[2].offset == offsetof(outer, n));

    REQUIRE(tag.fields[3].kind == field_kind::boolean);
    REQUIRE(tag.fields[3].offset == offsetof(outer, flag));
}

TEST_CASE("kernel/value | Text and enum fields read through their schema", "[kernel]")
{
    const auto& tag { tag_of<texty>() };

    REQUIRE(tag.fields.size() == 3);

    const auto& label { tag.fields[0] };
    REQUIRE(label.kind == field_kind::text);
    REQUIRE(std::string_view{ field_kind_text(label.kind) } == "text");
    REQUIRE(label.offset == offsetof(texty, label));

    const auto& state { tag.fields[1] };
    REQUIRE(state.kind == field_kind::enumeration);
    REQUIRE(std::string_view{ field_kind_text(state.kind) } == "enum");
    REQUIRE(state.offset == offsetof(texty, state));
    REQUIRE(state.size == sizeof(std::underlying_type_t<phase>));
    REQUIRE_FALSE(state.is_signed);
    REQUIRE(state.enumeration.size() == 3);

    texty object {};
    object.label = "hello";
    object.state = phase::active;
    object.count = 7;

    REQUIRE(read_text_field(&object, label) == "hello");

    REQUIRE(read_field(&object, state) == value { std::int64_t { 1 } });
    REQUIRE(std::string_view{ enum_name(state, 1) } == "active");
    REQUIRE(enum_name(state, 99) == nullptr);

    // Text has no kernel::value representation: read_field maps it to
    // monostate and the renderers special-case it through read_text_field.
    REQUIRE(read_field(&object, label) == value { std::monostate {} });
}

TEST_CASE("kernel/value | Described fields carry their kind, width and offset", "[kernel]")
{
    const auto& tag { tag_of<kinds>() };

    REQUIRE(tag.fields.size() == 12);

    const auto& flag { tag.fields[0] };
    REQUIRE(flag.kind == field_kind::boolean);
    REQUIRE(flag.size == sizeof(bool));
    REQUIRE(flag.offset == offsetof(kinds, flag));
    REQUIRE(flag.nested == nullptr);

    // Signed integers of every width (the value kind carries the signedness).
    const auto check_integer
    {
        [&](std::size_t index, field_kind kind, std::uint8_t size, std::uint32_t offset)
        {
            REQUIRE(tag.fields[index].kind == kind);
            REQUIRE(tag.fields[index].size == size);
            REQUIRE(tag.fields[index].offset == offset);
        }
    };

    check_integer(1, field_kind::integer, 1, offsetof(kinds, i8));
    check_integer(2, field_kind::integer, 2, offsetof(kinds, i16));
    check_integer(3, field_kind::integer, 4, offsetof(kinds, i32));
    check_integer(4, field_kind::integer, 8, offsetof(kinds, i64));

    check_integer(5, field_kind::unsigned_integer, 1, offsetof(kinds, u8));
    check_integer(6, field_kind::unsigned_integer, 2, offsetof(kinds, u16));
    check_integer(7, field_kind::unsigned_integer, 4, offsetof(kinds, u32));
    check_integer(8, field_kind::unsigned_integer, 8, offsetof(kinds, u64));

    check_integer(9, field_kind::floating, 4, offsetof(kinds, f32));
    check_integer(10, field_kind::floating, 8, offsetof(kinds, f64));

    REQUIRE(tag.fields[11].kind == field_kind::nested);
    REQUIRE(tag.fields[11].nested == &tag_of<inner>());
    REQUIRE(tag.fields[11].offset == offsetof(kinds, nested));
}

TEST_CASE("kernel/value | Optional members read engaged-or-absent", "[kernel]")
{
    const auto& tag { tag_of<maybe>() };

    REQUIRE(tag.fields.size() == 11);

    // Schema: payload kind and width, flagged optional, member offset.
    const auto check_optional
    {
        [&](std::size_t index, field_kind kind, std::uint8_t size, std::uint32_t offset)
        {
            REQUIRE(tag.fields[index].kind == kind);
            REQUIRE(tag.fields[index].size == size);
            REQUIRE(tag.fields[index].offset == offset);
            REQUIRE(tag.fields[index].optional);
            REQUIRE(tag.fields[index].nested == nullptr);
        }
    };

    check_optional(0, field_kind::boolean, 1, offsetof(maybe, opt_flag));
    check_optional(1, field_kind::integer, 1, offsetof(maybe, opt_i8));
    check_optional(2, field_kind::integer, 2, offsetof(maybe, opt_i16));
    check_optional(3, field_kind::integer, 4, offsetof(maybe, opt_i32));
    check_optional(4, field_kind::integer, 8, offsetof(maybe, opt_i64));
    check_optional(5, field_kind::unsigned_integer, 1, offsetof(maybe, opt_u8));
    check_optional(6, field_kind::unsigned_integer, 2, offsetof(maybe, opt_u16));
    check_optional(7, field_kind::unsigned_integer, 4, offsetof(maybe, opt_u32));
    check_optional(8, field_kind::unsigned_integer, 8, offsetof(maybe, opt_u64));
    check_optional(9, field_kind::floating, 4, offsetof(maybe, opt_f32));
    check_optional(10, field_kind::floating, 8, offsetof(maybe, opt_f64));

    // Engaged reads the payload with the scalar widening rules.
    const maybe engaged { true, -8, -800, -80000, -8000000000LL, 8, 800, 80000u, 8000000000ULL, 0.5f, 2.5 };

    const value engaged_want[]
    {
        value { true },
        value { std::int64_t { -8 } },
        value { std::int64_t { -800 } },
        value { std::int64_t { -80000 } },
        value { std::int64_t { -8000000000LL } },
        value { std::int64_t { 8 } },
        value { std::int64_t { 800 } },
        value { std::int64_t { 80000 } },
        value { static_cast<std::int64_t>(8000000000ULL) },
        value { 0.5 },
        value { 2.5 },
    };

    for (std::size_t i { 0 }; i < 11; ++i)
        REQUIRE(read_field(&engaged, tag.fields[i]) == engaged_want[i]);

    // Disengaged reads absent at every position: the struct stays positional
    // on the wire either way.
    const maybe silent {};

    for (std::size_t i { 0 }; i < 11; ++i)
        REQUIRE(read_field(&silent, tag.fields[i]) == value { std::monostate {} });
}

TEST_CASE("kernel/value | Inline values allocate nothing", "[kernel]")
{
    value_home home { 64 * 1024 };

    const auto baseline { home.pool.snapshot().used_bytes };

    const auto v { make_user_value<point>(&home, 3, 4) };
    REQUIRE(std::holds_alternative<user_inline>(v));
    REQUIRE(home.pool.snapshot().used_bytes == baseline);
}

TEST_CASE("kernel/value | Pool cells refcount and reclaim on the last release", "[kernel]")
{
    value_home home { 64 * 1024 };

    const auto baseline { home.pool.snapshot().used_bytes };

    int destroyed { 0 };

    {
        const auto v { make_user_value<owning>(&home, &destroyed) };
        REQUIRE(std::holds_alternative<user_ref>(v));
        REQUIRE(home.pool.owns(std::get<user_ref>(v).cell));
        REQUIRE(home.pool.snapshot().used_bytes > baseline);

        retain(v);
        release(v);
        REQUIRE(destroyed == 0);
        REQUIRE(home.pool.snapshot().used_bytes > baseline);

        // The last release retires the cell with its home; reclamation (here
        // driven manually — a program's tick does it at the next opportunity)
        // runs the destructor and frees the memory.
        release(v);
        REQUIRE(destroyed == 0);

        while (user_cell* cell { home.retired.pop() })
            reclaim(cell, &home.pool);

        REQUIRE(destroyed == 1);
        REQUIRE(home.pool.snapshot().used_bytes == baseline);
    }
}

TEST_CASE("kernel/value | Heap cells work without a pool", "[kernel]")
{
    int destroyed { 0 };

    user_cell* cell { make_user_cell<owning>(nullptr, &destroyed) };
    REQUIRE(cell->refs.load() == 1);

    retain(cell);
    release(cell);
    REQUIRE(destroyed == 0);

    release(cell);
    REQUIRE(destroyed == 1);
}

TEST_CASE("kernel/value | Scalars report no user type", "[kernel]")
{
    REQUIRE(type_of(value { std::monostate {} }) == nullptr);
    REQUIRE(type_of(value { true }) == nullptr);
    REQUIRE(type_of(value { std::int64_t { 3 } }) == nullptr);
    REQUIRE(type_of(value { 2.5 }) == nullptr);
}
