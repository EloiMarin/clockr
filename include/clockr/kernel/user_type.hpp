#pragma once

/// @file
/// User-value type identity and the optional debug schema (`describe<T>`,
/// `field`, `enum_field`). The kernel uses the identity for lifetime and the
/// schema only to render values in the debugger.

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

// The runtime identity and debug schema of a user value type.
//
// `kernel::value` is an erased variant: it can carry a `bool`, an `int64_t`, a
// `double`, or — since user values landed — a user struct, either inline (small
// trivially-copyable PODs) or by reference (a pool cell). Erasure means the
// kernel never needs the C++ type; but two things do need *some* runtime
// knowledge of it:
//
//   1. lifetime — releasing the last reference must run the struct's
//      destructor, so a cell carries a `destroy` function pointer; and
//   2. the debugger — the trace carries a struct's value, and the UI can only
//      render it if it is told which fields exist and where they live.
//
// Both are captured by `user_type`: a small, non-owning vtable built once per
// user type. `tag_of<T>()` returns the one for `T`; `describe<T>` is the
// opt-in that fills in the *fields* (the debug schema). A type that never
// specializes `describe` still works everywhere — it just renders as an
// anonymous `struct` in the debugger.
//
// Example:
//
//     struct loop_region { double start; double end; };
//
//     template<>
//     struct clockr::kernel::describe<loop_region>
//     {
//         static const user_type& type()
//         {
//             static const std::array fields
//             {
//                 field("start", &loop_region::start),
//                 field("end",   &loop_region::end),
//             };
//
//             static const user_type t { make_user_type<loop_region>("app.loop_region", fields) };
//
//             return t;
//         }
//     };
//
// Field offsets cannot be constant expressions in C++20 (member-pointer
// offsets are not), so a described type's `user_type` is initialized at static
// construction time. That is fine: the kernel and the constexpr program path
// only ever store *pointers* to `user_type`, never read through one at compile
// time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace clockr::kernel
{

struct user_type;

/// What a described field denotes, as far as the debugger is concerned.
///
/// `boolean`, `integer`, `unsigned_integer` and `floating` are read straight
/// from the payload at the field's offset; `nested` reads a child `user_type`
/// (pointed to by `field_desc::nested`) from the same offset and recurses;
/// `text` reads a `std::string` (see `read_text_field`); `enumeration` reads
/// an enum's underlying integer and maps it through `field_desc::enumeration`.
enum class field_kind : std::uint8_t
{
    boolean,
    integer,          // signed
    unsigned_integer,
    floating,
    nested,
    text,
    enumeration,
};

/// A name-as-string_view for the debugger wire (kept in the graph layer so the
/// codec never spells an enum switch twice).
constexpr
std::string_view
field_kind_text(field_kind kind) noexcept
{
    switch (kind)
    {
        case field_kind::boolean:
            return "bool";
        case field_kind::integer:
            return "int";
        case field_kind::unsigned_integer:
            return "uint";
        case field_kind::floating:
            return "double";
        case field_kind::nested:
            return "struct";
        case field_kind::text:
            return "text";
        case field_kind::enumeration:
            return "enum";
        default:
            return "";
    }
}

/// One entry of an enum field's name table: the enumerator's underlying value
/// and its spelling. `enum_field` borrows the table, so it must outlive every
/// program that uses the type (a static, in practice).
struct enum_entry
{
    std::int64_t value;
    const char* name;
};

/// One described field: a name, a byte offset into the struct, and how to read
/// it. `nested` is non-null exactly when `kind == field_kind::nested`; `size`
/// is the width in bytes the reader needs — the member width, except for
/// optional members, where it is the *payload* width (the reader maps
/// (kind, size) back to `optional<Payload>`, whose layout it never addresses
/// directly). `optional` marks an `optional<T>` member: engaged renders its
/// payload, disengaged reads as `monostate`, so the struct stays positional
/// on the wire either way.
struct field_desc
{
    const char* name { nullptr };
    std::uint32_t offset { 0 };
    field_kind kind { field_kind::integer };
    const user_type* nested { nullptr };
    std::uint8_t size { 0 };
    bool optional { false };

    // Non-empty iff `kind == field_kind::enumeration`: the enumerator name
    // table, in the order the user wrote it.
    std::span<const enum_entry> enumeration {};

    // Enumeration only: the underlying type's signedness, so `read_field`
    // sign-extends (or not) the same way the enumerator was written.
    bool is_signed { true };
};

/// The runtime vtable of a user value type.
///
/// Built once per type (`tag_of<T>()`) and immutable afterwards. `id` is a
/// stable hash of the registered name — 0 means "anonymous" (no `describe`),
/// which the debugger renders as a bare `struct`. `destroy` runs the payload's
/// destructor and is null for trivially-destructible types. `fields` is empty
/// when the type is not described.
struct user_type
{
    std::uint32_t id { 0 };
    const char* name { nullptr };
    std::uint32_t size { 0 };
    std::uint32_t align { 1 };
    void (*destroy)(void*) noexcept { nullptr };
    std::span<const field_desc> fields {};
};

static_assert(
    std::is_trivially_copyable_v<user_type>,
    "user_type is plain data: descriptors store pointers to it across TUs");

/// Opt-in debug schema for a user value type. Specialize it to describe the
/// fields the debugger should show; leave it undefined to stay anonymous.
template<typename T>
struct describe;

/// True when `describe<T>` provides a schema.
template<typename T>
concept described_c = requires { describe<T>::type(); };

/// True for `std::optional<T>`. The shell erases an optional to either its
/// payload or `monostate`; it is never itself a user value, so the `to_value`
/// / `from_value` dispatchers must route it through the optional overload
/// rather than treating it as a struct.
template<typename T>
struct is_optional : std::false_type {};

template<typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template<typename T>
constexpr bool is_optional_v = is_optional<T>::value;

/// A user value type: any class that is not one of the kernel's own
/// alternatives (`std::monostate`, `std::optional<T>`). This is deliberately a
/// catch-all — a struct works as a value with no registration at all; a
/// `describe` only enriches what the debugger can show.
template<typename T>
concept user_value_c = std::is_class_v<T>
    && (!std::same_as<T, std::monostate>)
    && (!std::same_as<T, std::nullopt_t>)
    && (!is_optional_v<T>);

namespace detail
{

template<typename T>
constexpr
auto
destroy_for() noexcept -> void (*)(void*) noexcept
{
    if constexpr (std::is_trivially_destructible_v<T>)
        return nullptr;
    else
        return +[](void* object) noexcept
        {
            std::destroy_at(static_cast<T*>(object));
        };
}

/// FNV-1a over the registered name: a stable, cross-run, cross-TU-order type
/// id for the debug wire. Collisions are checked when a program collects its
/// registry (a collision is a naming bug, caught loudly).
constexpr
std::uint32_t
fnv1a_hash(std::string_view text) noexcept
{
    std::uint32_t hash { 2166136261u };

    for (const char c : text)
    {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        hash *= 16777619u;
    }

    return hash;
}

/// Byte offset of a data member. Member-pointer offsets are not constant
/// expressions in C++20, so this is computed through a correctly-aligned
/// scratch object at static-initialization time (the usual reflection trick;
/// only the address is taken, no member is read).
template<typename C, typename M>
std::uint32_t
member_offset(M C::* member) noexcept
{
    alignas(C) static const std::byte scratch[sizeof(C)] {};
    const auto* object { reinterpret_cast<const C*>(scratch) };

    return static_cast<std::uint32_t>(
        reinterpret_cast<const std::byte*>(&(object->*member))
        - reinterpret_cast<const std::byte*>(object));
}

} // namespace detail

/// The `user_type` for `T`: the described one when available, otherwise a
/// stable per-`T` anonymous tag (id 0, no fields).
template<typename T>
requires user_value_c<T>
const user_type&
tag_of() noexcept
{
    if constexpr (described_c<T>)
    {
        return describe<T>::type();
    }
    else
    {
        static const user_type anonymous
        {
            0u,
            nullptr,
            static_cast<std::uint32_t>(sizeof(T)),
            static_cast<std::uint32_t>(alignof(T)),
            detail::destroy_for<T>(),
            {}
        };

        return anonymous;
    }
}

/// Describe an `optional<T>` member: engaged renders its payload under the
/// payload's kind, disengaged reads as `monostate` (see read_field). Scalar
/// payloads only — an optional user struct cannot name its payload type at
/// read time, so it fails here with the reason instead of deep in a reader.
/// To render one, carry the optionality one level up (an `optional<Struct>`
/// port already erases to payload-or-monostate on its own).
///
/// Declared before `field` so the call below resolves by ordinary lookup:
/// a later declaration would be invisible at `field`'s definition point,
/// and ADL does not reach this namespace from arbitrary user types.
template<typename C, typename P>
field_desc
optional_field(const char* name, std::optional<P> C::* member)
{
    using payload_t = std::remove_cv_t<P>;

    const auto offset { detail::member_offset(member) };
    const auto size { static_cast<std::uint8_t>(sizeof(payload_t)) };

    if constexpr (std::same_as<payload_t, bool>)
        return { name, offset, field_kind::boolean, nullptr, size, true };
    else if constexpr (std::integral<payload_t>)
        return
        {
            name,
            offset,
            std::is_signed_v<payload_t> ? field_kind::integer : field_kind::unsigned_integer,
            nullptr,
            size,
            true,
        };
    else if constexpr (std::floating_point<payload_t>)
        return { name, offset, field_kind::floating, nullptr, size, true };
    else if constexpr (user_value_c<payload_t>)
        static_assert(
            sizeof(payload_t) == 0,
            "describe<T>: an optional user-struct member has no field encoding yet — describe the struct unconditionally, or carry the optionality one level up (optional<Struct> port)");
    else
        static_assert(
            sizeof(payload_t) == 0,
            "describe<T>: an optional member must wrap bool, an integer, or a float");
}

/// Describe one field of a struct. The member type selects the kind:
/// `bool` → boolean, integral → integer, floating-point → floating,
/// `std::string` → text, and a user value type → nested (recursively rendering
/// its own schema). An `optional<T>` member takes the optional path above; an
/// enum member is rejected here (its enumerator names are needed) — use
/// `enum_field`. The name is borrowed, so it must outlive every program that
/// uses the type (a literal, in practice).
template<typename C, typename M>
field_desc
field(const char* name, M C::* member)
{
    using member_t = std::remove_cv_t<M>;

    const auto offset { detail::member_offset(member) };

    const auto size { static_cast<std::uint8_t>(sizeof(member_t)) };

    if constexpr (std::same_as<member_t, bool>)
        return { name, offset, field_kind::boolean, nullptr, size };
    else if constexpr (std::integral<member_t>)
        return
        {
            name,
            offset,
            std::is_signed_v<member_t> ? field_kind::integer : field_kind::unsigned_integer,
            nullptr,
            size,
        };
    else if constexpr (std::floating_point<member_t>)
        return { name, offset, field_kind::floating, nullptr, size };
    else if constexpr (std::same_as<member_t, std::string>)
        return { name, offset, field_kind::text, nullptr, size };
    else if constexpr (is_optional_v<member_t>)
        return optional_field(name, member);
    else if constexpr (std::is_enum_v<member_t>)
        static_assert(
            sizeof(member_t) == 0,
            "describe<T>: an enum member needs its enumerator names — describe it with enum_field(name, member, entries)");
    else if constexpr (user_value_c<member_t>)
        return { name, offset, field_kind::nested, &tag_of<member_t>(), size };
    else
        static_assert(
            sizeof(member_t) == 0,
            "describe<T>: a field must be bool, integral, floating-point, std::string, an optional of those, an enum (enum_field), or a user value type");
}

/// Describe an enum member, with the table that maps its underlying values to
/// their spellings. The entries are borrowed (a static, in practice) and are
/// scanned linearly when the debugger renders the field.
template<typename C, typename E, std::size_t N>
field_desc
enum_field(const char* name, E C::* member, const std::array<enum_entry, N>& entries)
{
    static_assert(std::is_enum_v<E>, "enum_field is for enum members");
    static_assert(N > 0, "enum_field needs at least one entry");

    using underlying_t = std::underlying_type_t<E>;

    return
    {
        name,
        detail::member_offset(member),
        field_kind::enumeration,
        nullptr,
        static_cast<std::uint8_t>(sizeof(underlying_t)),
        false,
        std::span<const enum_entry>{ entries.data(), entries.size() },
        std::is_signed_v<underlying_t>,
    };
}

/// Build the `user_type` for `T` from a name and its field table. The name and
/// the field array are borrowed; store the result in a static (see the example
/// at the top of this header).
template<typename T, std::size_t N>
user_type
make_user_type(const char* name, const std::array<field_desc, N>& fields)
{
    return user_type
    {
        detail::fnv1a_hash(name),
        name,
        static_cast<std::uint32_t>(sizeof(T)),
        static_cast<std::uint32_t>(alignof(T)),
        detail::destroy_for<T>(),
        std::span<const field_desc>{ fields.data(), fields.size() },
    };
}

} // namespace clockr::kernel
