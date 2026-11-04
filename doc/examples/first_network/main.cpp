// doc/examples/first_network/main.cpp — a network with state, stepped twice.
//
// The smallest program that shows clockr's whole story in one screenful:
// define a network, set an input, step, look at the output — then step again
// and watch state carry between ticks. It consumes the library exactly as an
// external project would: only public headers (the "clockr/..." include
// root) and only the public umbrella target (clockr::clockr). The
// newcomer walkthrough `doc/learn/your-first-network.md` quotes this file.

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <iostream>
#include <variant>

int
main()
{
    using namespace clockr;

    // The recipe. `>>` composes fragments left to right, like a pipeline;
    // the middle types must match and the compiler enforces that.
    const auto network {
        shell::input<double, kernel::clock_tag::ui>("alarm")
        >> shell::scan<double, double>(
            0.0, // the state's initial value: what the first tick sees
            [](const double& state, const double& value)
            {
                return state == 0.0 ? value : value * 2.0;
            })
        >> shell::output<double, kernel::clock_tag::ui>("tripped")
    };

    // Lower the recipe into a program: ports, regions, delay slots — all
    // data, no closures. Building either way is free of runtime graph
    // machinery; the engine consumed below treats ownership as transparent.
    kernel::program program { network };

    // A network is inert: nothing runs until the host steps it.
    program.set("alarm", kernel::value { 1.0 });

    // One step evaluates every node on the `ui` clock exactly once (L1/L2).
    // The network crossed no clocks, so one region — one step covers it all.
    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);

    const double first { std::get<double>(program.get("tripped")) };
    std::cout << "tick 1 read 1.0 -> sees the seed, trips with " << first << '\n';
    if (first != 1.0)
        return 1;

    // Same input again: `scan` folds the second tick through the previous
    // state instead of the seed.
    engine.step(program, kernel::clock_tag::ui);
    const double second { std::get<double>(program.get("tripped")) };
    std::cout << "tick 2 read 1.0 -> folds state, trips with " << second << '\n';
    if (second != 2.0)
        return 1;

    return 0;
}
