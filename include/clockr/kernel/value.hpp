#pragma once

// `kernel::value` — the single type that flows at runtime.
//
// The kernel is erased: it transports `bool`, `std::int64_t`, `double`, the
// payload-free `std::monostate` (used for "no event" and as the shell's void),
// and user structs. User structs take one of two forms:
//
//   * `user_inline` — a small trivially-copyable struct stored inline, with a
//     pointer to its `user_type`. Pure value semantics: no refcount, no pool,
//     and it crosses locks/links exactly like a scalar. This is the fast
//     path for POD snapshots (`loop_region { double, double }`, `beat_t`, …).
//   * `user_ref` — a pointer to a ref-counted pool cell (`user_cell`). Any
//     struct that is large or non-trivially-copyable (owns a buffer, has a
//     destructor) takes this path; lifetime is managed by the kernel at the
//     places a value escapes a tick.
//
// `value` must stay trivially copyable: the `latest` link copies its bytes
// word-by-word with atomics, the debug trace travels a lock-free queue, and the
// engine copies values per fan-in edge. A smart pointer member would break all
// three — hence the raw handle plus explicit retain/release.

#include "clockr/kernel/user_cell.hpp"
#include "clockr/kernel/user_type.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace clockr::kernel
{

/// Inline payload budget for `user_inline`. Deliberately small: it keeps
/// `sizeof(value)` at 32 bytes (see below) and nudges anything with real bulk
/// onto the ref-counted path.
inline constexpr std::size_t user_inline_capacity { 16 };
inline constexpr std::size_t user_inline_align { 8 };

/// A small trivially-copyable user struct stored by value. `type` is never
/// null for a value the shell produced; `bytes` holds the object
/// representation.
struct user_inline
{
    const user_type* type { nullptr };
    alignas(user_inline_align) std::array<std::byte, user_inline_capacity> bytes {};

    friend
    bool
    operator==(const user_inline& lhs, const user_inline& rhs) noexcept
    {
        return lhs.type == rhs.type && lhs.bytes == rhs.bytes;
    }
};

/// A ref-counted pool cell holding a user struct. Identity semantics: two
/// `user_ref`s are equal iff they point at the same cell.
struct user_ref
{
    user_cell* cell { nullptr };

    friend
    bool
    operator==(const user_ref& lhs, const user_ref& rhs) noexcept
    {
        return lhs.cell == rhs.cell;
    }
};

/// The only type that flows at runtime: a closed variant of unit
/// (`monostate`), `bool`, `int64_t`, `double`, a small inline user struct, or
/// a ref-counted user cell. It stays trivially copyable and 32 bytes (pinned
/// by the assertions below).
using value = std::variant<std::monostate, bool, std::int64_t, double, user_inline, user_ref>;

static_assert(
    std::is_trivially_copyable_v<value>,
    "kernel::value must stay trivially copyable: the latest link, the debug "
    "trace queue and the engine's per-edge copies all depend on it");

static_assert(
    std::is_trivially_destructible_v<value>,
    "kernel::value must stay trivially destructible; user-cell lifetime is "
    "managed explicitly by the kernel, never by the variant");

static_assert(
    sizeof(value) == 32,
    "kernel::value is expected to stay 32 bytes; update pool sizing and the "
    "link/recorder budget notes if this changes");

/// True when `T` is stored inline; false sends it to the ref-counted path.
template<typename T>
constexpr bool is_inlineable_user_v = user_value_c<T>
    && std::is_trivially_copyable_v<T>
    && (sizeof(T) <= user_inline_capacity)
    && (alignof(T) <= user_inline_align);

/// Construct a `value` holding `T{args...}`, inline when small, in a pool cell
/// otherwise. `home` may be null (heap; the dev/test path). Inline types ignore
/// it.
template<typename T, typename... Args>
requires std::constructible_from<T, Args...>
value
make_user_value([[maybe_unused]] const value_home* home, Args&&... args)
{
    static_assert(user_value_c<T>, "make_user_value is for user structs");

    if constexpr (is_inlineable_user_v<T>)
    {
        user_inline slot;
        slot.type = &tag_of<T>();

        const T object { std::forward<Args>(args)... };
        std::memcpy(slot.bytes.data(), &object, sizeof(T));

        return value { slot };
    }
    else
    {
        return value { user_ref { make_user_cell<T>(home, std::forward<Args>(args)...) } };
    }
}

/// Retain the cell behind `v`, if it is a `user_ref`.
inline
void
retain(const value& v) noexcept
{
    if (const auto* ref { std::get_if<user_ref>(&v) })
        retain(ref->cell);
}

/// Retain the cell behind `v` only if it is still live; true when there is
/// nothing to retain or the retain succeeded. The lock-free read path uses
/// this (see `conc::slot::load_retained`).
inline
bool
try_retain(const value& v) noexcept
{
    if (const auto* ref { std::get_if<user_ref>(&v) })
        return retain_if_live(ref->cell);

    return true;
}

/// Release the cell behind `v`, if it is a `user_ref`.
inline
void
release(const value& v) noexcept
{
    if (const auto* ref { std::get_if<user_ref>(&v) })
        release(ref->cell);
}

/// Overwrite `slot` with `incoming`, keeping the lifetime bookkeeping right:
/// release what was there, take a new reference for the arriving value (which
/// its source still owns). A no-op when the two are the same object.
inline
void
retain_assign(value& slot, const value& incoming) noexcept
{
    if (&slot == &incoming)
        return;

    release(slot);
    retain(incoming);
    slot = incoming;
}

inline
void
retain_assign(std::optional<value>& slot, const value& incoming) noexcept
{
    if (slot.has_value())
        release(*slot);

    retain(incoming);
    slot = incoming;
}

/// Overwrite `slot` with an already-owned value: the fresh reference moves in,
/// nothing is retained. Use for values just constructed (a tick's output, a
/// `set<T>` cell); the caller must not release `incoming` afterwards.
inline
void
transfer_assign(value& slot, const value& incoming) noexcept
{
    if (&slot == &incoming)
        return;

    release(slot);
    slot = incoming;
}

/// The `user_type` of a user value, or null for scalars. The debugger uses it
/// to render; the kernel uses it for nothing.
inline
const user_type*
type_of(const value& v) noexcept
{
    if (const auto* in { std::get_if<user_inline>(&v) })
        return in->type;

    if (const auto* ref { std::get_if<user_ref>(&v) })
        return ref->cell != nullptr ? ref->cell->type : nullptr;

    return nullptr;
}

/// The `user_type` a shell type denotes, or null for scalars. Optionals unwrap
/// to their payload, so an `optional<MyStruct>` port registers `MyStruct`.
///
/// `constexpr` for the scalar case (it returns null without touching
/// `tag_of`), which is what keeps scalar recipes constant-evaluable. For a
/// described struct the `tag_of` branch is runtime-only: `describe<T>::type()`
/// computes field offsets at static-initialization time, so a recipe over a
/// described user type cannot itself be a constant expression.
template<typename T>
constexpr
const user_type*
type_tag_or_null() noexcept
{
    if constexpr (is_optional_v<T>)
        return type_tag_or_null<typename T::value_type>();
    else if constexpr (user_value_c<T>)
    {
        // Only described types contribute a schema; an undescribed one still
        // works as a value (its cell carries the anonymous tag for the strict
        // read) but has nothing to send the debugger.
        if constexpr (described_c<T>)
            return &tag_of<T>();
        else
            return nullptr;
    }
    else
        return nullptr;
}

namespace detail
{
    template<typename T>
    T
    read_at(const void* base, const field_desc& field) noexcept
    {
        T value {};
        std::memcpy(&value, static_cast<const std::byte*>(base) + field.offset, sizeof(T));
        return value;
    }

    /// Read an optional member: engaged renders its payload, disengaged reads
    /// as `monostate`, so the struct stays positional either way. Reads
    /// through the optional object itself — its layout is
    /// implementation-defined, so the payload is never addressed directly.
    /// Same reinterpret idiom as shell::from_value for whole structs; the
    /// (kind, size) dispatch below names the exact payload type, mirroring
    /// the scalar mapping (bool direct, integrals widened to int64 with the
    /// schema's signedness, floats widened to double).
    template<typename T>
    value
    read_optional_at(const void* base, const field_desc& field) noexcept
    {
        const auto& opt
        {
            *reinterpret_cast<const std::optional<T>*>(
                static_cast<const std::byte*>(base) + field.offset)
        };

        if (!opt.has_value())
            return value { std::monostate {} };

        if constexpr (std::same_as<T, bool>)
            return value { *opt };
        else if constexpr (std::integral<T>)
            return value { static_cast<std::int64_t>(*opt) };
        else if constexpr (std::floating_point<T>)
            return value { static_cast<double>(*opt) };
        else
            return value { std::monostate {} }; // unreachable: field() rejects other payloads
    }
} // namespace detail

/// Read a `text` field (a `std::string` member) as a borrowed view into the
/// struct payload. The view is valid only while the payload is alive: the
/// trace sink copies it into its line on the ticking thread, and the codec
/// copies it into the proto while the recorder still retains the value's cell.
inline
std::string_view
read_text_field(const void* base, const field_desc& field) noexcept
{
    const auto& text
    {
        *reinterpret_cast<const std::string*>(
            static_cast<const std::byte*>(base) + field.offset)
    };

    return std::string_view { text };
}

/// The spelling of an `enumeration` field's value, or null when the value is
/// not in the field's name table (the caller then falls back to the integer).
inline
const char*
enum_name(const field_desc& field, std::int64_t value) noexcept
{
    for (const auto& entry : field.enumeration)
        if (entry.value == value)
            return entry.name;

    return nullptr;
}

/// Read one described scalar field from a struct payload, as a kernel value:
/// `bool`, `int64_t` (also for an unsigned field's bit pattern — the schema
/// carries the signedness) or `double`. An optional-marked field reads its
/// payload when engaged, `monostate` when disengaged. An `enumeration` field
/// reads as its underlying integer (the renderer maps it through the name
/// table). A `text` field, a `nested` field and an unknown kind read as
/// `monostate`; the caller handles text via `read_text_field` and recurses
/// into a nested field itself, via `field.nested` and the field's offset.
inline
value
read_field(const void* base, const field_desc& field) noexcept
{
    if (field.optional)
    {
        switch (field.kind)
        {
            case field_kind::boolean:
                return detail::read_optional_at<bool>(base, field);
            case field_kind::integer:
                switch (field.size)
                {
                    case 1: return detail::read_optional_at<std::int8_t>(base, field);
                    case 2: return detail::read_optional_at<std::int16_t>(base, field);
                    case 4: return detail::read_optional_at<std::int32_t>(base, field);
                    case 8: return detail::read_optional_at<std::int64_t>(base, field);
                    default: return value { std::monostate {} };
                }
            case field_kind::unsigned_integer:
                switch (field.size)
                {
                    case 1: return detail::read_optional_at<std::uint8_t>(base, field);
                    case 2: return detail::read_optional_at<std::uint16_t>(base, field);
                    case 4: return detail::read_optional_at<std::uint32_t>(base, field);
                    case 8: return detail::read_optional_at<std::uint64_t>(base, field);
                    default: return value { std::monostate {} };
                }
            case field_kind::floating:
                switch (field.size)
                {
                    case 4: return detail::read_optional_at<float>(base, field);
                    case 8: return detail::read_optional_at<double>(base, field);
                    default: return value { std::monostate {} };
                }
            case field_kind::nested:
                // Unreachable via field() (an optional user struct has no
                // encoding), defensive for hand-built schemas: absent.
                return value { std::monostate {} };
            case field_kind::text:
            case field_kind::enumeration:
                // No optional text/enum encoding; absent for hand-built
                // schemas.
                return value { std::monostate {} };
            default:
                return value { std::monostate {} };
        }
    }

    switch (field.kind)
    {
        case field_kind::boolean:
            return detail::read_at<bool>(base, field);
        case field_kind::integer:
            switch (field.size)
            {
                case 1: return std::int64_t { detail::read_at<std::int8_t>(base, field) };
                case 2: return std::int64_t { detail::read_at<std::int16_t>(base, field) };
                case 4: return std::int64_t { detail::read_at<std::int32_t>(base, field) };
                case 8: return detail::read_at<std::int64_t>(base, field);
                default: return value { std::monostate {} };
            }
        case field_kind::unsigned_integer:
            switch (field.size)
            {
                case 1: return std::int64_t { detail::read_at<std::uint8_t>(base, field) };
                case 2: return std::int64_t { detail::read_at<std::uint16_t>(base, field) };
                case 4: return std::int64_t { detail::read_at<std::uint32_t>(base, field) };
                case 8: return static_cast<std::int64_t>(detail::read_at<std::uint64_t>(base, field));
                default: return value { std::monostate {} };
            }
        case field_kind::floating:
            switch (field.size)
            {
                case 4: return static_cast<double>(detail::read_at<float>(base, field));
                case 8: return detail::read_at<double>(base, field);
                default: return value { std::monostate {} };
            }
        case field_kind::enumeration:
            switch (field.size)
            {
                case 1: return field.is_signed
                    ? std::int64_t { detail::read_at<std::int8_t>(base, field) }
                    : std::int64_t { detail::read_at<std::uint8_t>(base, field) };
                case 2: return field.is_signed
                    ? std::int64_t { detail::read_at<std::int16_t>(base, field) }
                    : std::int64_t { detail::read_at<std::uint16_t>(base, field) };
                case 4: return field.is_signed
                    ? std::int64_t { detail::read_at<std::int32_t>(base, field) }
                    : std::int64_t { detail::read_at<std::uint32_t>(base, field) };
                case 8: return field.is_signed
                    ? detail::read_at<std::int64_t>(base, field)
                    : static_cast<std::int64_t>(detail::read_at<std::uint64_t>(base, field));
                default: return value { std::monostate {} };
            }
        case field_kind::text:
            // Text has no kernel::value representation; the renderer reads it
            // through read_text_field before calling here.
            return value { std::monostate {} };
        case field_kind::nested:
            return value { std::monostate {} };
        default:
            return value { std::monostate {} };
    }
}

} // namespace clockr::kernel
