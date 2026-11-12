#pragma once

#include "clockr/shell/recipe.hpp"

#include <cstdint>
#include <stdexcept>

namespace replay_test
{

struct point
{
    int x { 0 };
    int y { 0 };
};

struct big_point
{
    std::int64_t values[8] {};
};

[[noreturn]] inline double
throw_tick()
{
    throw std::runtime_error { "tick" };
}

inline auto
network()
{
    return clockr::shell::parallel(
        clockr::shell::at<
            clockr::kernel::clock_tag::audio,
            clockr::conc::policy::latest>(
            clockr::shell::input<double, clockr::kernel::clock_tag::ui>(
                "level"
            ) >>
            clockr::shell::scan<double, double>(
                0.0,
                [](double state, double input) {
                    return state + input;
                }
            )
        ) >>
            clockr::shell::output<double, clockr::kernel::clock_tag::audio>(
                "total"
            ),
        clockr::shell::input<point, clockr::kernel::clock_tag::ui>("point") >>
            clockr::shell::output<point, clockr::kernel::clock_tag::ui>(
                "point_out"
            )
    );
}

inline auto
throwing_network()
{
    return clockr::shell::input<double, clockr::kernel::clock_tag::ui>(
               "level"
           ) >>
           clockr::shell::lift<double, double>([](double) -> double {
               return throw_tick();
           }) >>
           clockr::shell::output<double, clockr::kernel::clock_tag::ui>("out");
}

} // namespace replay_test
