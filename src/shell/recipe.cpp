#include "clockr/shell/recipe.hpp"

#include <span>

namespace clockr::shell::detail
{

// The delay trampoline: a delay node's tick is only ever called to produce
// its initial value (the engine drives delay state thereafter, see
// engine.hpp evaluate_nodes), so the tick ignores its inputs and returns the
// value held in the cell. One address serves every delay node. The returned
// value is retained: the engine's tick slot owns one reference (scalars are a
// no-op; a hand-written descriptor with a user-typed seed stays balanced).
kernel::value
delay_tick(const void* self, const kernel::tick_context&, std::span<const kernel::value>)
{
    const auto& seed { *static_cast<const kernel::value*>(self) };
    kernel::retain(seed);
    return seed;
}

} // namespace clockr::shell::detail
