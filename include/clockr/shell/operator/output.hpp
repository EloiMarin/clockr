#pragma once

// output: a sink port anchored on `thread` — its value is read on that clock.
// The tag doubles as a consistency check: if the chain feeding the output runs
// on a different clock, the build fails (crossing clocks without an explicit
// crossing is a build-time error).
//
// As with input ports, the name is borrowed, not copied: NUL-terminated
// storage outliving every program built from this recipe (eg., a string literal).

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

#include <string_view>

namespace clockr::shell
{

/// A sink port anchored on `Tag`, read by the host with `program::get`. The
/// tag is also a consistency check: a chain running on another clock fails to
/// build, because cross-clock flow must be explicit. The port name is
/// borrowed and must outlive every program built from this recipe.
template<typename T, kernel::clock_tag Tag>
constexpr
recipe<T, T, skeleton::tag_list<skeleton::output_t>, 1>
output(std::string_view port_name)
{
    return recipe<T, T, skeleton::tag_list<skeleton::output_t>, 1>
    {
        std::array<node, 1> { node { output_t(Tag, port_name, kernel::type_tag_or_null<T>()) } },
        0
    };
}

} // namespace clockr::shell
