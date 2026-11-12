#pragma once

/// @file
/// Typed value encoding for replay journals.
///
/// Scalars (`monostate`, `bool`, `int64_t`, `double`) are encoded directly.
/// User structs go through a host-registered codec, keyed by their
/// `kernel::user_type`: the journal therefore stores bytes, never a pool cell
/// owned by a live or dead program.

#include "clockr/kernel/clock.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/user_cell.hpp"
#include "clockr/kernel/user_type.hpp"
#include "clockr/kernel/value.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace clockr::replay
{

/// A user value's journal representation: its registry type id plus the raw
/// object bytes.
struct user_bytes
{
    std::uint32_t type_id { 0 };
    std::vector<std::byte> bytes;

    friend bool
    operator==(const user_bytes&, const user_bytes&) = default;
};

/// One journal value: a scalar alternative or encoded user bytes.
struct encoded_value
{
    std::variant<std::monostate, bool, std::int64_t, double, user_bytes>
        payload;
};

/// Semantic equality of two encoded values.
inline bool
encoded_value_equal(const encoded_value& lhs, const encoded_value& rhs)
{
    return lhs.payload == rhs.payload;
}

/// The encode/decode/compare operations a user type supplies to the journal.
class value_codec
{
public:
    virtual ~value_codec() = default;

    /// The stable id this codec uses on the wire.
    virtual std::uint32_t
    type_id() const noexcept = 0;

    /// Encode a live value; false with `error` when it is not this codec's
    /// type.
    virtual bool
    encode(
        const kernel::value& live,
        encoded_value& out,
        std::string& error
    ) const = 0;

    /// Decode into `target`'s region pool; the result owns one reference.
    virtual bool
    decode(
        const encoded_value& journal,
        kernel::program& target,
        kernel::clock_tag region,
        kernel::value& out,
        std::string& error
    ) const = 0;

    /// Compare a live value with a journal value semantically.
    virtual bool
    equal(
        const kernel::value& live,
        const encoded_value& journal,
        std::string& error
    ) const = 0;
};

namespace detail
{

/// Canonical object bytes for a trivially-copyable `T`.
///
/// A described type is copied field by field into a zeroed object, so padding
/// bytes (which the allocator or an aggregate initializer may leave
/// indeterminate) never reach the journal. Undescribed types, and types with
/// optional fields whose full object size the schema does not carry, keep the
/// raw object bytes.
template<typename T>
void
canonical_bytes(const void* source, std::vector<std::byte>& out)
{
    bool canonical { false };

    if constexpr (kernel::described_c<T>)
    {
        const auto& type { kernel::tag_of<T>() };
        bool has_optional { false };

        for (const auto& field : type.fields)
            if (field.optional)
                has_optional = true;

        if (!has_optional)
        {
            T cleared {};
            std::memset(&cleared, 0, sizeof(T));

            for (const auto& field : type.fields)
                if (field.size > 0)
                    std::memcpy(
                        reinterpret_cast<std::byte*>(&cleared) + field.offset,
                        static_cast<const std::byte*>(source) + field.offset,
                        field.size
                    );

            out.resize(sizeof(T));
            std::memcpy(out.data(), &cleared, sizeof(T));
            canonical = true;
        }
    }

    if (!canonical)
    {
        out.resize(sizeof(T));
        std::memcpy(out.data(), source, sizeof(T));
    }
}

/// Codec for a trivially-copyable user struct, stored by object bytes.
template<typename T>
class user_value_codec final : public value_codec
{
public:
    user_value_codec(std::uint32_t id, std::string name) :
        m_id { id },
        m_name { std::move(name) }
    {}

    std::uint32_t
    type_id() const noexcept override
    {
        return m_id;
    }

    bool
    encode(
        const kernel::value& live,
        encoded_value& out,
        std::string& error
    ) const override
    {
        const void* data { nullptr };

        if (const auto* in { std::get_if<kernel::user_inline>(&live) })
        {
            if (in->type != &kernel::tag_of<T>())
            {
                error = "value is not a '" + m_name + "'";
                return false;
            }

            data = in->bytes.data();
        }
        else if (const auto* ref { std::get_if<kernel::user_ref>(&live) })
        {
            if (ref->cell == nullptr || ref->cell->type != &kernel::tag_of<T>())
            {
                error = "value is not a '" + m_name + "'";
                return false;
            }

            data = kernel::user_payload(ref->cell);
        }
        else
        {
            error = "expected a '" + m_name + "' value";
            return false;
        }

        user_bytes encoded;
        encoded.type_id = m_id;
        canonical_bytes<T>(data, encoded.bytes);

        out.payload = std::move(encoded);

        return true;
    }

    bool
    decode(
        const encoded_value& journal,
        kernel::program& target,
        kernel::clock_tag region,
        kernel::value& out,
        std::string& error
    ) const override
    {
        const auto* bytes { std::get_if<user_bytes>(&journal.payload) };

        if (bytes == nullptr || bytes->type_id != m_id ||
            bytes->bytes.size() != sizeof(T))
        {
            error = "journal value is not a '" + m_name + "'";
            return false;
        }

        T object {};
        std::memcpy(&object, bytes->bytes.data(), sizeof(T));

        out = kernel::make_user_value<T>(target.region(region).home, object);

        return true;
    }

    bool
    equal(
        const kernel::value& live,
        const encoded_value& journal,
        std::string& error
    ) const override
    {
        encoded_value live_bytes;

        if (!encode(live, live_bytes, error))
            return false;

        const auto* lhs { std::get_if<user_bytes>(&live_bytes.payload) };
        const auto* rhs { std::get_if<user_bytes>(&journal.payload) };

        if (lhs == nullptr || rhs == nullptr || lhs->type_id != rhs->type_id)
        {
            error = "journal value is not a '" + m_name + "'";
            return false;
        }

        return lhs->bytes == rhs->bytes;
    }

private:
    std::uint32_t m_id;
    std::string m_name;
};

} // namespace detail

/// Host-owned codecs, looked up by journal type id or by live `user_type`.
class codec_registry
{
public:
    /// Register the codec for a trivially-copyable struct `T`.
    template<typename T>
    void
    register_user(std::string name)
    {
        static_assert(
            kernel::user_value_c<T>, "register_user is for user structs"
        );
        static_assert(
            std::is_trivially_copyable_v<T>,
            "replay codecs support trivially-copyable structs only"
        );
        static_assert(
            std::is_default_constructible_v<T>,
            "replay codecs require a default-constructible struct"
        );

        const auto* type { &kernel::tag_of<T>() };

        for (const auto& entry : m_entries)
            if (entry.type == type)
                return;

        std::uint32_t id { type->id };

        if (id == 0)
            id = kernel::detail::fnv1a_hash(name);

        for (const auto& entry : m_entries)
            if (entry.codec->type_id() == id)
                throw std::runtime_error { "replay codec id collision for '" +
                                           name + "'" };

        m_entries.push_back(
            entry_t { type,
                      std::make_unique<detail::user_value_codec<T>>(
                          id, std::move(name)
                      ) }
        );
    }

    /// The codec for a journal type id, or null.
    const value_codec*
    find(std::uint32_t type_id) const noexcept;

    /// The codec for a live user type, or null.
    const value_codec*
    find(const kernel::user_type& type) const noexcept;

    /// True when no user codecs are registered.
    bool
    empty() const noexcept
    {
        return m_entries.empty();
    }

private:
    struct entry_t
    {
        const kernel::user_type* type;
        std::unique_ptr<value_codec> codec;
    };

    std::vector<entry_t> m_entries;
};

/// Encode a live value; scalars are direct, user values use a registered codec.
bool
encode_value(
    const kernel::value& live,
    const codec_registry& registry,
    encoded_value& out,
    std::string& error
);

/// Decode a journal value into `target`'s region pool.
bool
decode_value(
    const encoded_value& journal,
    kernel::program& target,
    kernel::clock_tag region,
    const codec_registry& registry,
    kernel::value& out,
    std::string& error
);

/// Compare a live value with a journal value semantically.
bool
equal_value(
    const kernel::value& live,
    const encoded_value& journal,
    const codec_registry& registry,
    std::string& error
);

} // namespace clockr::replay
