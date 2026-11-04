#include "lab_common.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <emscripten/bind.h>

#include <cmath>
#include <memory>
#include <utility>

namespace
{

constexpr double tau { 6.283185307179586476925286766559 };

double
wrap_phase(double phase)
{
    if (!std::isfinite(phase))
        return 0.0;

    phase = std::fmod(phase, tau);

    return phase < 0.0 ? phase + tau : phase;
}

auto
flow_recipe()
{
    using namespace clockr;

    const auto dt { shell::share(shell::input<double, kernel::clock_tag::ui>("dt")) };
    const auto pointer_x { shell::share(shell::input<double, kernel::clock_tag::ui>("px")) };
    const auto pointer_y { shell::share(shell::input<double, kernel::clock_tag::ui>("py")) };

    const auto spin
    {
        shell::share(
            dt
            >> shell::scan<double, double>(
                   0.0,
                   [](const double& phase, const double& step)
                   {
                       return wrap_phase(phase + step * 0.55);
                   }))
    };

    const auto breathe
    {
        shell::share(
            dt
            >> shell::scan<double, double>(
                   1.7,
                   [](const double& phase, const double& step)
                   {
                       return wrap_phase(phase + step * 0.31);
                   }))
    };

    const auto sway
    {
        shell::share(
            dt
            >> shell::scan<double, double>(
                   3.1,
                   [](const double& phase, const double& step)
                   {
                       return wrap_phase(phase + step * 0.17);
                   }))
    };

    const auto wander
    {
        shell::share(
            dt
            >> shell::scan<double, double>(
                   0.4,
                   [](const double& phase, const double& step)
                   {
                       return wrap_phase(phase + step * 0.09);
                   }))
    };

    const auto smooth_x
    {
        shell::share(
            pointer_x
            >> shell::scan<double, double>(
                   0.5,
                   [](const double& previous, const double& x)
                   {
                       return previous + (x - previous) * 0.07;
                   }))
    };

    const auto smooth_y
    {
        shell::share(
            pointer_y
            >> shell::scan<double, double>(
                   0.5,
                   [](const double& previous, const double& y)
                   {
                       return previous + (y - previous) * 0.07;
                   }))
    };

    const auto angle
    {
        shell::zip(spin, smooth_x)
        >> shell::lift<std::pair<double, double>, double>(
               [](const double& phase, const double& x)
               {
                   return 0.62 * std::sin(phase)
                        + 0.34 * std::sin(phase * 0.5 + 1.3)
                        + (x - 0.5) * 1.15;
               })
        >> shell::output<double, kernel::clock_tag::ui>("angle")
    };

    const auto curl
    {
        shell::zip(breathe, spin, smooth_y)
        >> shell::lift<std::pair<std::pair<double, double>, double>, double>(
               [](const double& breathe_phase, const double& spin_phase, const double& y)
               {
                   return 1.05
                        + 0.42 * std::sin(breathe_phase)
                        + 0.22 * std::cos(spin_phase * 0.7)
                        + (y - 0.5) * 0.8;
               })
        >> shell::output<double, kernel::clock_tag::ui>("curl")
    };

    const auto scale
    {
        shell::zip(sway, breathe)
        >> shell::lift<std::pair<double, double>, double>(
               [](const double& sway_phase, const double& breathe_phase)
               {
                   return 1.85
                        + 0.55 * std::sin(sway_phase)
                        + 0.35 * std::cos(breathe_phase * 0.8);
               })
        >> shell::output<double, kernel::clock_tag::ui>("scale")
    };

    const auto speed
    {
        shell::zip(sway, breathe)
        >> shell::lift<std::pair<double, double>, double>(
               [](const double& sway_phase, const double& breathe_phase)
               {
                   return 96.0
                        + 30.0 * std::sin(sway_phase + 0.4)
                        + 14.0 * std::cos(breathe_phase * 0.7);
               })
        >> shell::output<double, kernel::clock_tag::ui>("speed")
    };

    const auto warp_x
    {
        shell::zip(smooth_x, wander)
        >> shell::lift<std::pair<double, double>, double>(
               [](const double& x, const double& wander_phase)
               {
                   return (x - 0.5) * 0.92 + 0.045 * std::sin(wander_phase);
               })
        >> shell::output<double, kernel::clock_tag::ui>("warpX")
    };

    const auto warp_y
    {
        shell::zip(smooth_y, wander)
        >> shell::lift<std::pair<double, double>, double>(
               [](const double& y, const double& wander_phase)
               {
                   return (y - 0.5) * 0.92 + 0.045 * std::cos(wander_phase * 1.3);
               })
        >> shell::output<double, kernel::clock_tag::ui>("warpY")
    };

    return shell::parallel(angle, curl, scale, speed, warp_x, warp_y);
}

double
unit(double value)
{
    if (!std::isfinite(value))
        return 0.5;

    return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
}

class FlowField
{
public:
    FlowField()
    {
        rebuild();
    }

    void
    tick(double dt)
    {
        if (!std::isfinite(dt) || dt < 0.0)
            dt = 0.0;
        else if (dt > 0.1)
            dt = 0.1;

        m_program->set("dt", dt);
        m_engine.step(*m_program, clockr::kernel::clock_tag::ui);
    }

    void
    setPointer(double x, double y)
    {
        m_program->set("px", unit(x));
        m_program->set("py", unit(y));
    }

    void
    reset()
    {
        rebuild();
    }

    double
    angle() const
    {
        return lab::read_double(*m_program, "angle");
    }

    double
    curl() const
    {
        return lab::read_double(*m_program, "curl");
    }

    double
    scale() const
    {
        return lab::read_double(*m_program, "scale");
    }

    double
    speed() const
    {
        return lab::read_double(*m_program, "speed");
    }

    double
    warpX() const
    {
        return lab::read_double(*m_program, "warpX");
    }

    double
    warpY() const
    {
        return lab::read_double(*m_program, "warpY");
    }

private:
    void
    rebuild()
    {
        m_program = std::make_unique<clockr::kernel::program>(flow_recipe());
        m_program->set("dt", 0.0);
        m_program->set("px", 0.5);
        m_program->set("py", 0.5);

        m_engine.step(*m_program, clockr::kernel::clock_tag::ui);
        m_engine.step(*m_program, clockr::kernel::clock_tag::ui);
    }

    std::unique_ptr<clockr::kernel::program> m_program;
    clockr::kernel::release_engine m_engine;
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(flow_field)
{
    emscripten::class_<FlowField>("FlowField")
        .constructor()
        .function("tick", &FlowField::tick)
        .function("setPointer", &FlowField::setPointer)
        .function("reset", &FlowField::reset)
        .function("angle", &FlowField::angle)
        .function("curl", &FlowField::curl)
        .function("scale", &FlowField::scale)
        .function("speed", &FlowField::speed)
        .function("warpX", &FlowField::warpX)
        .function("warpY", &FlowField::warpY);
}
#pragma clang diagnostic pop
