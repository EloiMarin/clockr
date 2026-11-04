#include "message.hpp"

#include <stdexcept>
#include <string>

namespace clockr::debug
{

namespace
{
    // A described struct cannot contain itself by value, so this is only a
    // guard against a hand-written schema with a cycle.
    constexpr int max_struct_depth { 8 };

    void
    encode_field(const void* base, const kernel::field_desc& field, pb::Value& out, int depth);

    void
    encode_struct(const kernel::user_type* type, const void* payload, pb::StructValue& out, int depth)
    {
        out.set_type_id(type != nullptr ? type->id : 0);

        if (type == nullptr || payload == nullptr || depth >= max_struct_depth)
            return;

        for (const auto& field : type->fields)
            encode_field(payload, field, *out.add_fields(), depth);
    }

    void
    encode_field(const void* base, const kernel::field_desc& field, pb::Value& out, int depth)
    {
        if (field.kind == kernel::field_kind::nested)
        {
            encode_struct(
                field.nested,
                static_cast<const std::byte*>(base) + field.offset,
                *out.mutable_struct_value(),
                depth + 1);
            return;
        }

        if (field.kind == kernel::field_kind::text)
        {
            out.set_string_value(std::string { kernel::read_text_field(base, field) });
            return;
        }

        // A scalar field reads as a kernel value; the schema's width and
        // signedness are applied by read_field.
        const auto field_value { kernel::read_field(base, field) };

        if (field.kind == kernel::field_kind::enumeration)
        {
            // Prefer the enumerator's spelling; an unknown value travels as
            // its underlying integer.
            if (const auto* value { std::get_if<std::int64_t>(&field_value) })
                if (const auto* name { kernel::enum_name(field, *value) })
                {
                    out.set_string_value(name);
                    return;
                }
        }

        out = to_message(field_value);
    }
} // namespace

pb::Value
to_message(const kernel::value& value)
{
    pb::Value message;

    if (const auto* b { std::get_if<bool>(&value) })
        message.set_bool_value(*b);
    else if (const auto* i { std::get_if<std::int64_t>(&value) })
        message.set_int_value(*i);
    else if (const auto* d { std::get_if<double>(&value) })
        message.set_double_value(*d);
    else if (const auto* in { std::get_if<kernel::user_inline>(&value) })
        encode_struct(in->type, in->bytes.data(), *message.mutable_struct_value(), 0);
    else if (const auto* ref { std::get_if<kernel::user_ref>(&value) })
    {
        const kernel::user_cell* cell { ref->cell };

        encode_struct(
            cell != nullptr ? cell->type : nullptr,
            cell != nullptr ? kernel::user_payload(cell) : nullptr,
            *message.mutable_struct_value(),
            0);
    }
    else
        message.set_unit(true); // monostate

    return message;
}

kernel::value
from_message(const pb::Value& message)
{
    switch (message.kind_case())
    {
        case pb::Value::kUnit:
            return kernel::value { std::monostate {} };
        case pb::Value::kBoolValue:
            return kernel::value { message.bool_value() };
        case pb::Value::kIntValue:
            return kernel::value { static_cast<std::int64_t>(message.int_value()) };
        case pb::Value::kDoubleValue:
            return kernel::value { message.double_value() };
        case pb::Value::kStructValue:
            // One-way: a struct needs its type and a pool home, which the
            // consumer side does not have. Read back as monostate.
            return kernel::value { std::monostate {} };
        case pb::Value::kStringValue:
            // One-way too: text has no kernel::value representation.
            return kernel::value { std::monostate {} };
        case pb::Value::KIND_NOT_SET:
        default:
            return kernel::value { std::monostate {} };
    }
}

std::string
to_message(kernel::clock_tag clock)
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
to_message(kernel::node_kind kind)
{
    switch (kind)
    {
        case kernel::node_kind::input:
            return "input";
        case kernel::node_kind::output:
            return "output";
        case kernel::node_kind::internal:
            return "internal";
        case kernel::node_kind::delay:
            return "delay";
        default:
            return "unknown";
    }
}

namespace
{
    pb::EventKind
    kind_of(kernel::trace_event_kind kind)
    {
switch (kind)
    {
        case kernel::trace_event_kind::region_step_begin:
            return pb::REGION_EVAL_BEGIN;
        case kernel::trace_event_kind::region_step_end:
            return pb::REGION_EVAL_END;
        case kernel::trace_event_kind::node_tick_begin:
            return pb::NODE_EVAL_BEGIN;
        case kernel::trace_event_kind::node_tick_end:
            return pb::NODE_EVAL_END;
        case kernel::trace_event_kind::node_tick_throw:
            return pb::NODE_EVAL_THROW;
        default:
            return pb::REGION_EVAL_BEGIN;
    }
    }

    kernel::trace_event_kind
    kind_from(pb::EventKind kind)
    {
        if (kind == pb::REGION_EVAL_END)
            return kernel::trace_event_kind::region_step_end;

        if (kind == pb::NODE_EVAL_BEGIN)
            return kernel::trace_event_kind::node_tick_begin;

        if (kind == pb::NODE_EVAL_END)
            return kernel::trace_event_kind::node_tick_end;

        if (kind == pb::NODE_EVAL_THROW)
            return kernel::trace_event_kind::node_tick_throw;

        return kernel::trace_event_kind::region_step_begin;
    }
} // namespace

pb::TraceEvent
to_message(const kernel::trace_event& event, std::uint64_t stamp_ns)
{
    pb::TraceEvent message;

    message.set_sequence(event.sequence);
    message.set_stamp_ns(stamp_ns);
    message.set_kind(kind_of(event.kind));
    message.set_node_id(event.node_id);
    message.set_clock(to_message(event.clock));
    message.set_epoch(event.epoch);

    switch (event.kind)
    {
        case kernel::trace_event_kind::node_tick_end:
            *message.mutable_value() = to_message(event.payload);
            message.set_duration_ns(event.duration_ns);
            break;
        case kernel::trace_event_kind::node_tick_throw:
            message.set_duration_ns(event.duration_ns);
            break;
        case kernel::trace_event_kind::region_step_end:
            if (const auto* threw { std::get_if<bool>(&event.payload) })
                message.set_threw(*threw);
            message.set_duration_ns(event.duration_ns);
            break;
        case kernel::trace_event_kind::region_step_begin:
        case kernel::trace_event_kind::node_tick_begin:
        default:
            break;
    }

    return message;
}

kernel::trace_event
from_message(const pb::TraceEvent& message)
{
    kernel::trace_event event {};

    event.kind = kind_from(message.kind());
    event.clock = kernel::clock_tag::audio;
    event.epoch = message.epoch();
    event.node_id = message.node_id();
    event.sequence = message.sequence();
    event.payload = kernel::value { std::monostate {} };
    event.duration_ns = message.duration_ns();

    // The thread tag round-trips as a name; parsing it back keeps the decode
    // side honest without depending on the enum's numeric stability.
    const auto& thread { message.clock() };

    if (thread == "ui")
        event.clock = kernel::clock_tag::ui;
    else if (thread == "worker")
        event.clock = kernel::clock_tag::worker;
    else
        event.clock = kernel::clock_tag::audio;

    if (message.kind() == pb::NODE_EVAL_END && message.has_value())
        event.payload = from_message(message.value());

    if (message.kind() == pb::REGION_EVAL_END)
        event.payload = kernel::value { message.threw() };

    return event;
}

} // namespace clockr::debug
