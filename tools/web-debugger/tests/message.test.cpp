#include "server/message.hpp"

#include "clockr/kernel/release_engine.hpp"

#include "clockr/kernel/program.hpp"

#include "clockr/shell/descriptor_storage.hpp"
#include "clockr/shell/recipe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace clockr;

namespace
{
    auto
    sum_network()
    {
        using kernel::clock_tag;

        return shell::zip(
                   shell::input<std::int64_t, clock_tag::ui>("a"),
                   shell::input<std::int64_t, clock_tag::ui>("b"))
            >> shell::lift<std::pair<std::int64_t, std::int64_t>, std::int64_t>(
                   [](const std::int64_t& x, const std::int64_t& y) { return x + y; })
            >> shell::output<std::int64_t, clock_tag::ui>("out");
    }
} // namespace

TEST_CASE("debugger/message | Values round-trip through the schema", "[debugger][message]")
{
    using debug::from_message;
    using debug::to_message;

    REQUIRE(from_message(to_message(kernel::value { std::monostate {} })) == kernel::value { std::monostate {} });
    REQUIRE(from_message(to_message(kernel::value { true })) == kernel::value { true });
    REQUIRE(from_message(to_message(kernel::value { false })) == kernel::value { false });
    REQUIRE(from_message(to_message(kernel::value { std::int64_t { -42 } })) == kernel::value { std::int64_t { -42 } });
    REQUIRE(from_message(to_message(kernel::value { 2.5 })) == kernel::value { 2.5 });
}

TEST_CASE("debugger/message | A graph description carries nodes, fan-in and ports", "[debugger][message]")
{
    const auto network { sum_network() };
    kernel::program program { network };

    const auto description { debug::describe(program) };
    const auto graph { program.descriptor() };

    REQUIRE(description.nodes_size() == static_cast<int>(graph.nodes.size()));

    // The first two nodes are the inputs, in declaration order, with names.
    REQUIRE(description.nodes(0).kind() == "input");
    REQUIRE(description.nodes(0).port() == "a");
    REQUIRE(description.nodes(1).port() == "b");

    // The lift's positional fan-in is preserved (a then b).
    const auto lift_id { static_cast<int>(graph.nodes.size()) - 2 };
    REQUIRE(description.nodes(lift_id).label() == "lift");
    REQUIRE(description.nodes(lift_id).fan_in_size() == 2);
    REQUIRE(description.nodes(lift_id).fan_in(0) == 0);
    REQUIRE(description.nodes(lift_id).fan_in(1) == 1);

    REQUIRE(description.inputs_size() == 2);
    REQUIRE(description.inputs(0).name() == "a");
    REQUIRE(description.inputs(1).name() == "b");

    REQUIRE(description.outputs_size() == 1);
    REQUIRE(description.outputs(0).name() == "out");
}

TEST_CASE("debugger/message | A graph description carries crossings", "[debugger][message]")
{
    using kernel::clock_tag;
    using conc::policy;

    const auto network
    {
        shell::at<clock_tag::audio, conc::policy::latest>(
            shell::input<double, clock_tag::ui>("bpm"))
            >> shell::output<double, clock_tag::audio>("out")
    };

    kernel::program program { network };

    const auto description { debug::describe(program) };

    REQUIRE(program.topology().crossings.size() == 1);
    REQUIRE(description.crossings_size() == 1);
    REQUIRE(description.crossings(0).policy() == "latest");
}

TEST_CASE("debugger/message | Trace events round-trip", "[debugger][message]")
{
    using kernel::trace_event;
    using kernel::trace_event_kind;

    // node_tick_end carries a value and the measured evaluation time.
    {
        const trace_event event {
            trace_event_kind::node_tick_end,
            kernel::clock_tag::audio,
            7,
            3,
            11,
            kernel::value { std::int64_t { 5 } },
            12'345,
        };

        const auto message { debug::to_message(event, 1234) };
        REQUIRE(message.duration_ns() == 12'345);

        const auto decoded { debug::from_message(message) };

        REQUIRE(decoded.kind == event.kind);
        REQUIRE(decoded.clock == event.clock);
        REQUIRE(decoded.epoch == event.epoch);
        REQUIRE(decoded.node_id == event.node_id);
        REQUIRE(decoded.sequence == event.sequence);
        REQUIRE(decoded.payload == event.payload);
        REQUIRE(decoded.duration_ns == event.duration_ns);
    }

    // node_tick_throw carries the failed evaluation's measured time too.
    {
        const trace_event event {
            trace_event_kind::node_tick_throw,
            kernel::clock_tag::audio,
            7,
            3,
            12,
            kernel::value { std::monostate {} },
            678,
        };

        const auto decoded { debug::from_message(debug::to_message(event, 1234)) };

        REQUIRE(decoded.kind == event.kind);
        REQUIRE(decoded.duration_ns == 678);
    }

    // region_step_end carries the threw flag and the whole-tick time.
    {
        const trace_event event {
            trace_event_kind::region_step_end,
            kernel::clock_tag::worker,
            2,
            0,
            9,
            kernel::value { true },
            1'500'000,
        };

        const auto message { debug::to_message(event, 42) };
        REQUIRE(message.threw());
        REQUIRE(message.duration_ns() == 1'500'000);

        const auto decoded { debug::from_message(message) };

        REQUIRE(decoded.kind == event.kind);
        REQUIRE(decoded.clock == event.clock);
        REQUIRE(decoded.payload == kernel::value { true });
        REQUIRE(decoded.duration_ns == 1'500'000);
    }

    // node_tick_begin carries no value.
    {
        const trace_event event {
            trace_event_kind::node_tick_begin,
            kernel::clock_tag::ui,
            1,
            4,
            0,
            kernel::value { std::monostate {} },
            0,
        };

        const auto decoded { debug::from_message(debug::to_message(event, 0)) };

        REQUIRE(decoded.kind == event.kind);
        REQUIRE(decoded.node_id == event.node_id);
        REQUIRE(std::holds_alternative<std::monostate>(decoded.payload));
    }
}

namespace
{
    struct inner
    {
        std::int32_t n { 0 };
        bool flag { false };
    };

    struct outer
    {
        double start { 0.0 };
        double end { 0.0 };
        inner nested {};
        std::uint8_t flags { 0 };
        float gain { 0.0f };
        std::int16_t delta { 0 };
    };

    struct inner2
    {
        std::int32_t m { 0 };
    };

    // Nests two distinct described types: one directly named type, three
    // reachable types, on a two-node network.
    struct composite
    {
        inner first {};
        inner2 second {};
    };

    // Not described: a placeholder struct.
    struct anonymous
    {
        std::int64_t x { 0 };
    };

    // An optional member: engaged renders its payload, disengaged absent.
    struct opt_sample
    {
        double level { 0.0 };
        std::optional<double> seek {};
    };

    // A text member and an enum member: the kinds with no kernel::value
    // representation.
    enum class phase : std::uint8_t
    {
        idle,
        active,
        done,
    };

    struct labelled
    {
        std::string label;
        phase state { phase::idle };
    };
} // namespace

namespace clockr::kernel
{
template<>
struct describe<inner>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("n", &inner::n),
            field("flag", &inner::flag),
        };
        static const user_type t { make_user_type<inner>("test.inner", fields) };

        return t;
    }
};

template<>
struct describe<inner2>
{
    static const user_type& type()
    {
        static const std::array fields { field("m", &inner2::m) };
        static const user_type t { make_user_type<inner2>("test.inner2", fields) };

        return t;
    }
};

template<>
struct describe<composite>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("first", &composite::first),
            field("second", &composite::second),
        };
        static const user_type t { make_user_type<composite>("test.composite", fields) };

        return t;
    }
};

template<>
struct describe<outer>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("start", &outer::start),
            field("end", &outer::end),
            field("nested", &outer::nested),
            field("flags", &outer::flags),
            field("gain", &outer::gain),
            field("delta", &outer::delta),
        };
        static const user_type t { make_user_type<outer>("test.outer", fields) };

        return t;
    }
};

template<>
struct describe<opt_sample>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("level", &opt_sample::level),
            field("seek", &opt_sample::seek),
        };
        static const user_type t { make_user_type<opt_sample>("test.opt_sample", fields) };

        return t;
    }
};

template<>
struct describe<labelled>
{
    static const user_type& type()
    {
        static const std::array phases
        {
            enum_entry { 0, "idle" },
            enum_entry { 1, "active" },
            enum_entry { 2, "done" },
        };
        static const std::array fields
        {
            field("label", &labelled::label),
            enum_field("state", &labelled::state, phases),
        };
        static const user_type t { make_user_type<labelled>("test.labelled", fields) };

        return t;
    }
};
} // namespace clockr::kernel

TEST_CASE("debugger/message | A graph description carries user type schemas", "[debugger][message]")
{
    using kernel::clock_tag;

    const auto network
    {
        shell::input<outer, clock_tag::ui>("o") >> shell::output<outer, clock_tag::ui>("out")
    };
    kernel::program program { network };

    const auto description { debug::describe(program) };

    // The described type and the type it nests are both listed, so the client
    // can render nested fields.
    REQUIRE(description.types_size() == 2);

    const auto* outer_schema { static_cast<const debug::pb::TypeSchema*>(nullptr) };

    for (const auto& schema : description.types())
        if (schema.name() == "test.outer")
            outer_schema = &schema;

    REQUIRE(outer_schema != nullptr);
    REQUIRE(outer_schema->id() == kernel::tag_of<outer>().id);
    REQUIRE(outer_schema->fields_size() == 6);

    REQUIRE(outer_schema->fields(0).name() == "start");
    REQUIRE(outer_schema->fields(0).kind() == "double");
    REQUIRE(outer_schema->fields(2).name() == "nested");
    REQUIRE(outer_schema->fields(2).kind() == "struct");
    REQUIRE(outer_schema->fields(2).type_id() == kernel::tag_of<inner>().id);
    REQUIRE(outer_schema->fields(3).name() == "flags");
    REQUIRE(outer_schema->fields(3).kind() == "uint");
    REQUIRE(outer_schema->fields(4).kind() == "double");
    REQUIRE(outer_schema->fields(5).kind() == "int");
}

TEST_CASE("debugger/message | A compiled descriptor carries nested type schemas", "[debugger][message]")
{
    using kernel::clock_tag;

    // Two nodes, three reachable types. The descriptor must stay within its
    // node-count type budget (it carries only the directly named type) and the
    // program builds the complete registry from the field schemas — this used
    // to write one past the descriptor's type array.
    const auto network
    {
        shell::input<composite, clock_tag::ui>("c") >> shell::output<composite, clock_tag::ui>("out")
    };

    const auto net { shell::compile(network) };
    kernel::program borrowed { net.descriptor() };

    REQUIRE(borrowed.types().size() == 3);

    const auto description { debug::describe(borrowed) };
    REQUIRE(description.types_size() == 3);

    std::vector<std::string> names;
    for (const auto& schema : description.types())
        names.push_back(schema.name());

    REQUIRE(std::ranges::find(names, "test.composite") != names.end());
    REQUIRE(std::ranges::find(names, "test.inner") != names.end());
    REQUIRE(std::ranges::find(names, "test.inner2") != names.end());
}

TEST_CASE("debugger/message | A struct value encodes its fields by schema", "[debugger][message]")
{
    using kernel::clock_tag;

    const auto network
    {
        shell::input<outer, clock_tag::ui>("o") >> shell::output<outer, clock_tag::ui>("out")
    };
    kernel::program program { network };

    const auto payload
    {
        kernel::make_user_value<outer>(
            program.region(clock_tag::ui).home,
            1.0,
            4.0,
            inner { 42, true },
            std::uint8_t { 7 },
            0.5f,
            std::int16_t { -300 })
    };

    REQUIRE(std::holds_alternative<kernel::user_ref>(payload)); // large enough to pool

    const auto message { debug::to_message(payload) };

    REQUIRE(message.kind_case() == debug::pb::Value::kStructValue);

    const auto& value { message.struct_value() };
    REQUIRE(value.type_id() == kernel::tag_of<outer>().id);
    REQUIRE(value.fields_size() == 6);

    REQUIRE(value.fields(0).double_value() == 1.0);
    REQUIRE(value.fields(1).double_value() == 4.0);

    // Nested struct, recursively.
    REQUIRE(value.fields(2).kind_case() == debug::pb::Value::kStructValue);
    REQUIRE(value.fields(2).struct_value().type_id() == kernel::tag_of<inner>().id);
    REQUIRE(value.fields(2).struct_value().fields(0).int_value() == 42);
    REQUIRE(value.fields(2).struct_value().fields(1).bool_value());

    // Width and signedness come from the schema: a uint8 reads zero-extended,
    // an int16 sign-extended, a float widens.
    REQUIRE(value.fields(3).int_value() == 7);
    REQUIRE(value.fields(4).double_value() == 0.5);
    REQUIRE(value.fields(5).int_value() == -300);

    // The test made this cell by hand; the program does not own it.
    kernel::release(payload);
}

TEST_CASE("debugger/message | Text and enum fields encode as their spelling", "[debugger][message]")
{
    using kernel::clock_tag;

    const auto network
    {
        shell::input<labelled, clock_tag::ui>("l") >> shell::output<labelled, clock_tag::ui>("out")
    };
    kernel::program program { network };

    // The schema advertises the two kinds.
    const auto description { debug::describe(program) };
    const debug::pb::TypeSchema* schema { nullptr };

    for (const auto& candidate : description.types())
        if (candidate.name() == "test.labelled")
            schema = &candidate;

    REQUIRE(schema != nullptr);
    REQUIRE(schema->fields_size() == 2);
    REQUIRE(schema->fields(0).kind() == "text");
    REQUIRE(schema->fields(1).kind() == "enum");

    const auto payload
    {
        kernel::make_user_value<labelled>(
            program.region(clock_tag::ui).home, std::string { "hello" }, phase::active)
    };
    REQUIRE(std::holds_alternative<kernel::user_ref>(payload));

    const auto message { debug::to_message(payload) };
    REQUIRE(message.kind_case() == debug::pb::Value::kStructValue);

    const auto& value { message.struct_value() };
    REQUIRE(value.fields(0).kind_case() == debug::pb::Value::kStringValue);
    REQUIRE(value.fields(0).string_value() == "hello");
    REQUIRE(value.fields(1).kind_case() == debug::pb::Value::kStringValue);
    REQUIRE(value.fields(1).string_value() == "active");

    kernel::release(payload);

    // An enumerator not in the table falls back to its underlying integer.
    const auto unknown
    {
        kernel::make_user_value<labelled>(
            program.region(clock_tag::ui).home, std::string { "x" }, static_cast<phase>(9))
    };

    const auto unknown_value { debug::to_message(unknown).struct_value() };
    REQUIRE(unknown_value.fields(1).kind_case() == debug::pb::Value::kIntValue);
    REQUIRE(unknown_value.fields(1).int_value() == 9);

    kernel::release(unknown);
}

TEST_CASE("debugger/message | An anonymous struct is flagged as a placeholder", "[debugger][message]")
{
    using kernel::clock_tag;

    const auto network
    {
        shell::input<anonymous, clock_tag::ui>("a") >> shell::output<anonymous, clock_tag::ui>("out")
    };
    kernel::program program { network };

    // No `describe`: the program registers nothing for it.
    REQUIRE(program.types().empty());

    const auto payload
    {
        kernel::make_user_value<anonymous>(program.region(clock_tag::ui).home, 5)
    };

    const auto message { debug::to_message(payload) };

    REQUIRE(message.kind_case() == debug::pb::Value::kStructValue);
    REQUIRE(message.struct_value().type_id() == 0);
    REQUIRE(message.struct_value().fields_size() == 0);

    kernel::release(payload);
}

TEST_CASE("debugger/message | A select-example request parses by hand-built bytes", "[debugger][message]")
{
    // ClientMessage { select_example = 3 } holding SelectExample { name = 1 }.
    // Mirrors web/schema.js encodeSelectExample byte for byte (7 bytes; the
    // literal's trailing NUL is not part of the frame).
    const std::string bytes { "\x1a\x05\x0a\x03" "daw", 7 };

    debug::pb::ClientMessage message;
    REQUIRE(message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
    REQUIRE(message.has_select_example());
    REQUIRE(message.select_example().name() == "daw");
}

TEST_CASE("debugger/message | An optional field encodes engaged-or-absent", "[debugger][message]")
{
    using kernel::clock_tag;

    const auto network
    {
        shell::input<opt_sample, clock_tag::ui>("o") >> shell::output<opt_sample, clock_tag::ui>("out")
    };
    kernel::program program { network };

    // Engaged: the payload rides under its own kind at the field's position.
    const auto engaged
    {
        kernel::make_user_value<opt_sample>(
            program.region(clock_tag::ui).home, 1.5, std::optional<double> { 2.5 })
    };

    const auto engaged_message { debug::to_message(engaged) };
    REQUIRE(engaged_message.struct_value().fields_size() == 2);
    REQUIRE(engaged_message.struct_value().fields(0).double_value() == 1.5);
    REQUIRE(engaged_message.struct_value().fields(1).double_value() == 2.5);
    kernel::release(engaged);

    // Disengaged: monostate at the same position, so the struct stays
    // positional and the page renders it absent.
    const auto silent
    {
        kernel::make_user_value<opt_sample>(
            program.region(clock_tag::ui).home, 1.5, std::optional<double> {})
    };

    const auto silent_message { debug::to_message(silent) };
    REQUIRE(silent_message.struct_value().fields_size() == 2);
    REQUIRE(silent_message.struct_value().fields(0).double_value() == 1.5);
    REQUIRE(silent_message.struct_value().fields(1).kind_case() == debug::pb::Value::kUnit);
    kernel::release(silent);
}
