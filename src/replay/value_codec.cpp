#include "clockr/replay/value_codec.hpp"

#include <string>
#include <variant>

namespace clockr::replay
{

namespace
{

std::string
type_name(const kernel::user_type& type)
{
    return type.name != nullptr ? std::string { type.name }
                                : std::string { "<anonymous>" };
}

} // namespace

const value_codec*
codec_registry::find(std::uint32_t type_id) const noexcept
{
    for (const auto& entry : m_entries)
        if (entry.codec->type_id() == type_id)
            return entry.codec.get();

    return nullptr;
}

const value_codec*
codec_registry::find(const kernel::user_type& type) const noexcept
{
    for (const auto& entry : m_entries)
        if (entry.type == &type)
            return entry.codec.get();

    return nullptr;
}

bool
encode_value(
    const kernel::value& live,
    const codec_registry& registry,
    encoded_value& out,
    std::string& error
)
{
    if (std::holds_alternative<std::monostate>(live))
    {
        out.payload = std::monostate {};
        return true;
    }

    if (const auto* boolean { std::get_if<bool>(&live) })
    {
        out.payload = *boolean;
        return true;
    }

    if (const auto* integer { std::get_if<std::int64_t>(&live) })
    {
        out.payload = *integer;
        return true;
    }

    if (const auto* real { std::get_if<double>(&live) })
    {
        out.payload = *real;
        return true;
    }

    const auto* type { kernel::type_of(live) };

    if (type == nullptr)
    {
        error = "user value carries no type";
        return false;
    }

    const auto* codec { registry.find(*type) };

    if (codec == nullptr)
    {
        error = "no replay codec registered for user type '" +
                type_name(*type) + "'";
        return false;
    }

    return codec->encode(live, out, error);
}

bool
decode_value(
    const encoded_value& journal,
    kernel::program& target,
    kernel::clock_tag region,
    const codec_registry& registry,
    kernel::value& out,
    std::string& error
)
{
    if (std::holds_alternative<std::monostate>(journal.payload))
    {
        out = kernel::value { std::monostate {} };
        return true;
    }

    if (const auto* boolean { std::get_if<bool>(&journal.payload) })
    {
        out = kernel::value { *boolean };
        return true;
    }

    if (const auto* integer { std::get_if<std::int64_t>(&journal.payload) })
    {
        out = kernel::value { *integer };
        return true;
    }

    if (const auto* real { std::get_if<double>(&journal.payload) })
    {
        out = kernel::value { *real };
        return true;
    }

    if (const auto* user { std::get_if<user_bytes>(&journal.payload) })
    {
        const auto* codec { registry.find(user->type_id) };

        if (codec == nullptr)
        {
            error = "no replay codec registered for journal type id " +
                    std::to_string(user->type_id);
            return false;
        }

        return codec->decode(journal, target, region, out, error);
    }

    error = "unrecognised journal value";
    return false;
}

bool
equal_value(
    const kernel::value& live,
    const encoded_value& journal,
    const codec_registry& registry,
    std::string& error
)
{
    if (std::holds_alternative<user_bytes>(journal.payload))
    {
        const auto* type { kernel::type_of(live) };

        if (type == nullptr)
        {
            error = "live value is not a user struct";
            return false;
        }

        const auto* codec { registry.find(*type) };

        if (codec == nullptr)
        {
            error = "no replay codec registered for user type '" +
                    type_name(*type) + "'";
            return false;
        }

        return codec->equal(live, journal, error);
    }

    encoded_value live_encoded;

    if (!encode_value(live, registry, live_encoded, error))
        return false;

    if (!encoded_value_equal(live_encoded, journal))
    {
        error = "journal value differs";
        return false;
    }

    return true;
}

} // namespace clockr::replay
