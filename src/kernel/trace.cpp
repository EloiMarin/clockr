#include "clockr/kernel/trace.hpp"

#include "clockr/kernel/user_type.hpp"

#include <string>
#include <string_view>

namespace clockr::kernel
{

namespace
{
    constexpr int max_struct_depth { 4 };

    // Append `text` as the contents of a JSON string: quotes, backslashes and
    // control characters are escaped, so a describe name with odd bytes cannot
    // break the JSON-lines trace around it.
    void
    append_json_escaped(std::string& out, std::string_view text)
    {
        for (const char c : text)
        {
            switch (c)
            {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        constexpr char hex[] = "0123456789abcdef";

                        out += "\\u00";
                        out += hex[(static_cast<unsigned char>(c) >> 4) & 0xf];
                        out += hex[static_cast<unsigned char>(c) & 0xf];
                    }
                    else
                    {
                        out += c;
                    }
            }
        }
    }

    std::string
    struct_text(const kernel::user_type* type, const void* payload, int depth);

    // A quoted string for *inside* the struct's JSON string, so the quotes
    // themselves are escaped (`\"hi\"`): the struct renders as one JSON string
    // value, and an unescaped inner quote would break the line.
    std::string
    json_string(std::string_view text)
    {
        std::string out { "\\\"" };
        append_json_escaped(out, text);
        out += "\\\"";
        return out;
    }

    std::string
    struct_field_text(const void* base, const kernel::field_desc& field, int depth)
    {
        if (field.kind == kernel::field_kind::nested)
            return struct_text(
                field.nested,
                static_cast<const std::byte*>(base) + field.offset,
                depth + 1);

        if (field.kind == kernel::field_kind::text)
            return json_string(kernel::read_text_field(base, field));

        const auto field_value { kernel::read_field(base, field) };

        if (field.kind == kernel::field_kind::enumeration)
        {
            if (const auto* value { std::get_if<std::int64_t>(&field_value) })
                if (const auto* name { kernel::enum_name(field, *value) })
                    return json_string(name);
        }

        return value_text(field_value);
    }

    // A struct renders as a JSON string (`"name{field=value, ...}"`), so a
    // JSON-lines trace stays parseable; scalars render as their JSON literals.
    // Names are escaped; nesting stops at max_struct_depth and reports the
    // struct's name without fields (or the literal `struct` when even the
    // type is unknown).
    std::string
    struct_text(const kernel::user_type* type, const void* payload, int depth)
    {
        if (type == nullptr || payload == nullptr || depth > max_struct_depth)
            return "\"struct\"";

        std::string text;
        text += '"';
        append_json_escaped(text, type->name != nullptr ? std::string_view { type->name } : std::string_view { "struct" });
        text += '{';

        for (std::size_t i { 0 }; i < type->fields.size(); ++i)
        {
            const auto& field { type->fields[i] };

            if (i != 0)
                text += ", ";

            append_json_escaped(text, field.name != nullptr ? std::string_view { field.name } : std::string_view { "?" });
            text += '=';
            text += struct_field_text(payload, field, depth);
        }

        text += "}\"";
        return text;
    }

    std::string_view
    kind_name(trace_event_kind kind)
    {
        switch (kind)
        {
            case trace_event_kind::region_step_begin:
                return "region_step_begin";
            case trace_event_kind::region_step_end:
                return "region_step_end";
            case trace_event_kind::node_tick_begin:
                return "node_tick_begin";
            case trace_event_kind::node_tick_end:
                return "node_tick_end";
            case trace_event_kind::node_tick_throw:
                return "node_tick_throw";
            default:
                return "unknown";
        }
    }
} // namespace

text_trace_sink::text_trace_sink(std::string& target) :
    m_target { target }
{}

void
text_trace_sink::on_event(const trace_event& event)
{
    m_target += event_to_line(event);
    m_target += '\n';
}

void
cout_trace_sink::on_event(const trace_event& event)
{
    std::cout << "[trace_sink] " << event_to_line(event) << std::endl;
}

bool
is_present(const kernel::value& value) noexcept
{
    return !std::holds_alternative<std::monostate>(value);
}

std::string
value_text(const kernel::value& value)
{
    if (const auto* b { std::get_if<bool>(&value) })
        return *b ? "true" : "false";

    if (const auto* i { std::get_if<std::int64_t>(&value) })
        return std::to_string(*i);

    if (const auto* d { std::get_if<double>(&value) })
        return std::to_string(*d);

    if (const auto* in { std::get_if<kernel::user_inline>(&value) })
        return struct_text(in->type, in->bytes.data(), 0);

    if (const auto* ref { std::get_if<kernel::user_ref>(&value) })
    {
        const kernel::user_cell* cell { ref->cell };

        return struct_text(
            cell != nullptr ? cell->type : nullptr,
            cell != nullptr ? kernel::user_payload(cell) : nullptr,
            0);
    }

    return {};
}

std::string
clock_text(kernel::clock_tag clock)
{
    switch (clock)
    {
        case kernel::clock_tag::audio:
            return "audio";
        case kernel::clock_tag::ui:
            return "ui";
        case kernel::clock_tag::worker:
            return "worker";
        default:
            return "unknown";
    }
}

std::string
event_to_line(const trace_event& event)
{
    std::string line;
    line.reserve(160);

    line += "{\"kind\":\"";
    line += kind_name(event.kind);
    line += "\",\"clock\":\"";
    line += clock_text(event.clock);
    line += "\",\"epoch\":";
    line += std::to_string(event.epoch);
    line += ",\"node\":";
    line += std::to_string(event.node_id);
    line += ",\"seq\":";
    line += std::to_string(event.sequence);

    if (event.kind == trace_event_kind::node_tick_end && is_present(event.payload))
    {
        line += ",\"value\":";
        line += value_text(event.payload);
    }

    if (event.kind == trace_event_kind::node_tick_end
        || event.kind == trace_event_kind::node_tick_throw)
    {
        line += ",\"duration_ns\":";
        line += std::to_string(event.duration_ns);
    }

    if (event.kind == trace_event_kind::region_step_end && std::holds_alternative<bool>(event.payload))
    {
        line += ",\"threw\":";
        line += std::get<bool>(event.payload) ? "true" : "false";
        line += ",\"duration_ns\":";
        line += std::to_string(event.duration_ns);
    }

    line += "}";

    return line;
}

} // namespace clockr::kernel
