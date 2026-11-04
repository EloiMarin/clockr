#pragma once

// input: a source port anchored on `thread` — whoever feeds this port
// (the app, via program::set) runs on that clock. Its A is std::monostate —
// the "void" of the value world: it needs no upstream value, the engine
// feeds it from the input span.
//
// Storage rule follows the type, no parameter needed: plain ports persist
// whatever was last set (levels), while optional-typed ports are
// fire-and-forget — the engine reverts them to disengaged after the tick
// that consumes them. Want a sticky Maybe? Compose an explicit scan
// (stepper); persistence must be a visible node, never a port mode.
//
// The name is borrowed, not copied: its bytes must be NUL-terminated storage
// that outlives every program built from this recipe (eg., a string literal).

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

namespace clockr::shell
{

/// A source port anchored on `Tag`, fed by the host with `program::set`.
///
/// A plain `out_t` persists its last value (a level). An `optional<out_t>`
/// port is fire-and-forget: the engine reverts it to disengaged after the tick
/// that consumes it (compose `stepper` for a sticky event). The port name
/// is borrowed and must outlive every program built from this recipe.
template<typename out_t, kernel::clock_tag Tag>
constexpr
recipe<std::monostate, out_t, skeleton::tag_list<skeleton::input_t>, 1>
input(std::string_view port_name)
{
    return recipe<std::monostate, out_t, skeleton::tag_list<skeleton::input_t>, 1>
    {
        std::array<node, 1> { node { input_t(Tag, port_name, is_optional_v<out_t>, kernel::type_tag_or_null<out_t>()) } },
        0
    };
}

} // namespace clockr::shell
