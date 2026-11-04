#pragma once

// dawmulator — the canonical construction shared by the interactive web
// shell (demo/daw/shell) and the debugger's `daw` example
// (tools/web-debugger/main.cpp): one recipe, one set of default levels, one
// init tick. Anything that wants "the DAWmulator's program" builds it here,
// so the shells cannot drift apart on score, levels, or init order —
// drivers differ per shell (the page waits for gestures, the debugger
// presses play and ticks endlessly), but the program is one.
//
// The defaults are the powered-on-but-paused console: the intent ports carry
// safe levels and the ui region has ticked once before any audio tick — a
// latest link holds monostate until its producer's first tick, and the
// bundle lift reads the crossed levels (the same ordering the C++ tests
// assume).

#include "daw/network.hpp"

#include "clockr/kernel/engine.hpp"
#include "clockr/kernel/program.hpp"

namespace daw
{

/// The DAWmulator's network: P1 transport + scheduler + voice over the
/// scripted score (note 60 at beat 3.5, one beat long). By value, like every
/// recipe: each shell constructs its own program from it.
inline auto
dawmulator_recipe()
{
    return net::app(net::score { 3.5, 1.0, 60 });
}

/// Level the DAWmulator's intent ports to their defaults and tick the ui
/// region once, through any engine — release in the web shell, debug under
/// the debugger. L5 parity is what makes the two views interchangeable, and
/// demo.test.cpp pins it on this network.
inline void
init_dawmulator(clockr::kernel::program& prog, clockr::kernel::engine& eng)
{
    prog.set("playing", false);
    prog.set("bpm", 120.0);
    prog.set<LoopRegion>("loop", 0.0, 4.0, true);
    prog.set("master", 0.8);
    eng.step(prog, clockr::kernel::clock_tag::ui);
}

} // namespace daw
