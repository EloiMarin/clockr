// The Shape of a Hold — a held spring follows the pointer.
//
// ui clock: rawX/rawY are the raw pointer levels. Each axis feeds a critically
// damped spring whose whole state (position + velocity) is a `scan`; the
// renderer draws the raw pointer as a faint ghost and the held point as the
// solid, tactile one. Reset rebuilds the program from the same recipe.

#include "lab_common.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <emscripten/bind.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace
{

using namespace clockr;

using kernel::clock_tag;

// Both are exactly two doubles, so they stay on the kernel's inline user-value
// path (no pool allocation per tick).
struct spring_in
{
    double target { 0.5 };
    double dt { 0.0 };
};

struct spring_state
{
    double x { 0.5 };
    double v { 0.0 };
};

template<typename target_recipe_t, typename dt_recipe_t>
auto
spring_chain(const target_recipe_t& target, const dt_recipe_t& dt)
{
    using namespace clockr::shell;

    return zip(target, dt)
        >> lift<std::pair<double, double>, spring_in>(
               [](const double& t, const double& d)
               { return spring_in { t, d }; })
        >> scan<spring_in, spring_state>(
               spring_state { 0.5, 0.0 },
               [](const spring_state& s, const spring_in& in)
               {
                   // Critically damped: a = w^2 (target - x) - 2 w v.
                   constexpr double omega { 9.0 };
                   const double d { std::clamp(in.dt, 0.0, 0.05) };
                   const double accel { omega * omega * (in.target - s.x) - 2.0 * omega * s.v };
                   const double v { s.v + accel * d };
                   const double x { s.x + v * d };

                   return spring_state { x, v };
               });
}

auto
shape_of_hold_recipe()
{
    using namespace clockr::shell;

    const auto raw_x { share(input<double, clock_tag::ui>("rawX")) };
    const auto raw_y { share(input<double, clock_tag::ui>("rawY")) };
    const auto dt { share(input<double, clock_tag::ui>("dt")) };

    const auto spring_x { spring_chain(raw_x, dt) };
    const auto spring_y { spring_chain(raw_y, dt) };

    const auto held_x
    {
        spring_x
            >> lift<spring_state, double>([](const spring_state& s) { return s.x; })
    };
    const auto held_y
    {
        spring_y
            >> lift<spring_state, double>([](const spring_state& s) { return s.x; })
    };

    return parallel(
        raw_x >> output<double, clock_tag::ui>("rawX"),
        raw_y >> output<double, clock_tag::ui>("rawY"),
        held_x >> output<double, clock_tag::ui>("heldX"),
        held_y >> output<double, clock_tag::ui>("heldY"));
}

class ShapeOfHold
{
public:
    ShapeOfHold()
    {
        reset();
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        const double step { std::isfinite(dt) ? std::clamp(dt, 0.0, 0.05) : 1.0 / 60.0 };

        m_program->set("dt", step);
        m_engine.step(*m_program, clock_tag::ui);
    }

    void
    setPointer(double x, double y)
    {
        if (!m_program)
            return;

        m_program->set("rawX", std::clamp(std::isfinite(x) ? x : 0.5, 0.0, 1.0));
        m_program->set("rawY", std::clamp(std::isfinite(y) ? y : 0.5, 0.0, 1.0));
    }

    void
    reset()
    {
        m_program.emplace(shape_of_hold_recipe());

        m_program->set("rawX", 0.5);
        m_program->set("rawY", 0.5);
        m_program->set("dt", 1.0 / 60.0);

        // Settle the spring at rest so the first frame has a held point.
        for (int i { 0 }; i < 120; ++i)
            m_engine.step(*m_program, clock_tag::ui);
    }

    double
    rawX() const
    {
        return lab::read_double(*m_program, "rawX");
    }

    double
    rawY() const
    {
        return lab::read_double(*m_program, "rawY");
    }

    double
    heldX() const
    {
        return lab::read_double(*m_program, "heldX");
    }

    double
    heldY() const
    {
        return lab::read_double(*m_program, "heldY");
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;
};

} // namespace

EMSCRIPTEN_BINDINGS(lab_shape_of_hold)
{
    emscripten::class_<ShapeOfHold>("ShapeOfHold")
        .constructor()
        .function("tick", &ShapeOfHold::tick)
        .function("setPointer", &ShapeOfHold::setPointer)
        .function("reset", &ShapeOfHold::reset)
        .function("rawX", &ShapeOfHold::rawX)
        .function("rawY", &ShapeOfHold::rawY)
        .function("heldX", &ShapeOfHold::heldX)
        .function("heldY", &ShapeOfHold::heldY);
}
