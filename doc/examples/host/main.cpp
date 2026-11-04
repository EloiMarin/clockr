// doc/examples/host/main.cpp — the smallest possible clockr host.
//
// It consumes the library exactly as an external project would: only public
// headers (the "clockr/..." include root) and only the public umbrella
// target (clockr::clockr). Build it in-tree with CLOCKR_BUILD_EXAMPLES,
// or copy it into your own project — see doc/how-to/consuming.md.

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"
#include "clockr/version.hpp"

#include <iostream>
#include <variant>

int
main()
{
    using namespace clockr;

    const auto network {
        shell::input<double, kernel::clock_tag::ui>("in")
        >> shell::lift<double, double>(
            [](const double& value)
            {
                return value * 2.0;
            })
        >> shell::output<double, kernel::clock_tag::ui>("out")
    };

    kernel::program program { network };
    program.set("in", kernel::value { 21.0 });

    kernel::release_engine engine;
    engine.step(program, kernel::clock_tag::ui);

    const double result { std::get<double>(program.get("out")) };
    if (result != 42.0)
    {
        std::cout << "unexpected result: " << result << '\n';
        return 1;
    }

    std::cout << "clockr " << version::str << ": 21 * 2 = " << result << '\n';
    return 0;
}
