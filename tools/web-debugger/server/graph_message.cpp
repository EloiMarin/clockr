#include "message.hpp"

#include "clockr/kernel/topology.hpp"

#include <cassert>
#include <string>

namespace clockr::debug
{

namespace
{
    std::string
    combinator_of(const kernel::descriptor_view& graph, kernel::node_id id)
    {
        const auto label { kernel::label(graph, id) };

        return label.combinator ? std::string { label.combinator } : std::string {};
    }

    std::string
    port_of(const kernel::descriptor_view& graph, kernel::node_id id)
    {
        const auto label { kernel::label(graph, id) };

        return label.port_name ? std::string { label.port_name } : std::string {};
    }
} // namespace

std::string
node_label_text(const kernel::descriptor_view& graph, kernel::node_id id)
{
    return combinator_of(graph, id);
}

std::string
node_port_text(const kernel::descriptor_view& graph, kernel::node_id id)
{
    return port_of(graph, id);
}

pb::GraphDescription
describe(const kernel::program& program)
{
    const auto graph { program.descriptor() };

    pb::GraphDescription description;

    for (const auto& node : nodes(graph))
    {
        const auto id { kernel::id(node) };
        auto* out { description.add_nodes() };

        out->set_id(id);
        out->set_kind(to_message(kernel::kind(node)));
        out->set_clock(to_message(kernel::clock(node)));
        out->set_label(combinator_of(graph, id));
        out->set_port(port_of(graph, id));

        // Positional fan-in order: the order the shell connected the node's
        // inputs, preserved by the flat fan_in span.
        for (const auto producer : fan_in(graph, id))
            out->add_fan_in(producer);
    }

    for (const auto& point : program.topology().crossings)
    {
        auto* out { description.add_crossings() };

        out->set_from(point.from);
        out->set_to(point.to);
        out->set_policy(point.policy == conc::policy::queued ? "queued" : "latest");
    }

    for (std::size_t i { 0 }; i < graph.input_ids.size(); ++i)
    {
        auto* out { description.add_inputs() };

        out->set_node_id(graph.input_ids[i]);
        out->set_name(graph.input_names[i] ? graph.input_names[i] : "");
    }

    for (std::size_t i { 0 }; i < graph.output_ids.size(); ++i)
    {
        auto* out { description.add_outputs() };

        out->set_node_id(graph.output_ids[i]);
        out->set_name(graph.output_names[i] ? graph.output_names[i] : "");
    }

    // The user value types this program can carry, so the client can render a
    // StructValue's fields by name. Anonymous types (id 0, no `describe`) are
    // not listed: their values render as a bare `struct`.
    for (const auto* type : program.types())
    {
        auto* schema { description.add_types() };

        schema->set_id(type->id);
        schema->set_name(type->name != nullptr ? type->name : "");

        for (const auto& field : type->fields)
        {
            auto* field_message { schema->add_fields() };

            field_message->set_name(field.name != nullptr ? field.name : "");
            field_message->set_kind(std::string { kernel::field_kind_text(field.kind) });

            if (field.kind == kernel::field_kind::nested && field.nested != nullptr)
                field_message->set_type_id(field.nested->id);
        }
    }

    return description;
}

} // namespace clockr::debug
