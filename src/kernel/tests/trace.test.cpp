#include "clockr/kernel/trace.hpp"

#include "clockr/kernel/user_type.hpp"
#include "clockr/kernel/value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace
{
    // Big enough for the ref-counted path, so both the inline and the cell
    // rendering branches are worth covering.
    struct weird
    {
        double level { 0.0 };
        double spare_a { 0.0 };
        double spare_b { 0.0 };
    };

    enum class state : std::uint8_t
    {
        idle,
        active,
    };

    struct labelled
    {
        std::string label;
        state phase { state::idle };
    };
} // namespace

template<>
struct clockr::kernel::describe<weird>
{
    static const user_type& type()
    {
        static const std::array fields
        {
            field("le\"vel\\x", &weird::level),
        };
        static const user_type t { make_user_type<weird>("we\"ird\\type", fields) };

        return t;
    }
};

template<>
struct clockr::kernel::describe<labelled>
{
    static const user_type& type()
    {
        static const std::array phases
        {
            enum_entry { 0, "idle" },
            enum_entry { 1, "active" },
        };
        static const std::array fields
        {
            field("label", &labelled::label),
            enum_field("phase", &labelled::phase, phases),
        };
        static const user_type t { make_user_type<labelled>("labelled", fields) };

        return t;
    }
};

using namespace clockr;

TEST_CASE("kernel/trace | Text and enum fields render their spelling", "[kernel]")
{
    labelled object {};
    object.label = "hi";
    object.phase = state::active;

    const auto payload { kernel::make_user_value<labelled>(nullptr, object) };
    REQUIRE(std::holds_alternative<kernel::user_ref>(payload));

    const auto line
    {
        kernel::event_to_line(kernel::trace_event {
            kernel::trace_event_kind::node_tick_end,
            kernel::clock_tag::ui,
            1,
            0,
            1,
            payload,
            0,
        })
    };

    // The text is quoted (with the quotes escaped for the enclosing JSON
    // string) and the enum renders its enumerator name, not its integer.
    REQUIRE(line.find("label=\\\"hi\\\"") != std::string::npos);
    REQUIRE(line.find("phase=\\\"active\\\"") != std::string::npos);

    kernel::release(payload);
}

TEST_CASE("kernel/trace | Struct names are escaped in the JSON-lines trace", "[kernel]")
{
    const auto payload { kernel::make_user_value<weird>(nullptr, 2.5) };
    REQUIRE(std::holds_alternative<kernel::user_ref>(payload));

    const auto line
    {
        kernel::event_to_line(kernel::trace_event {
            kernel::trace_event_kind::node_tick_end,
            kernel::clock_tag::ui,
            1,
            0,
            1,
            payload,
            0,
        })
    };

    // The type and field names carry a quote and a backslash; both must be
    // escaped so the line stays parseable JSON (`we\"ird\\type{le\"vel\\x=…}`).
    REQUIRE(line.find("we\\\"ird\\\\type") != std::string::npos);
    REQUIRE(line.find("le\\\"vel\\\\x") != std::string::npos);
    REQUIRE(line.find("2.500000") != std::string::npos);

    kernel::release(payload);
}
