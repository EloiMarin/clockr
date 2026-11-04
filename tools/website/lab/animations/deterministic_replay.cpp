#include "lab_common.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <emscripten/bind.h>

#include <cmath>
#include <optional>
#include <utility>

namespace
{

using namespace clockr;

using kernel::clock_tag;
using pair_wire = std::pair<double, double>;

constexpr double phase_offset { 2.0943951023931953 };

auto
deterministic_replay_recipe()
{
    using namespace clockr::shell;

    const auto dt { share(input<double, clock_tag::ui>("dt")) };

    const auto phase_a
    {
        share(
            dt
            >> scan<double, double>(
                   0.0,
                   [](const double& phase, const double& step)
                   { return phase + step * 1.20; }))
    };
    const auto phase_b
    {
        share(
            dt
            >> scan<double, double>(
                   0.0,
                   [](const double& phase, const double& step)
                   { return phase + step * 3.60; }))
    };

    const auto x
    {
        zip(phase_a, phase_b)
            >> lift<pair_wire, double>(
                   [](const double& a, const double& b)
                   {
                       return 0.5 + 0.31 * std::sin(a)
                            + 0.12 * std::sin(b + 0.65);
                   })
    };
    const auto y
    {
        zip(phase_a, phase_b)
            >> lift<pair_wire, double>(
                   [](const double& a, const double& b)
                   {
                       return 0.5 + 0.31 * std::cos(2.0 * a + 0.35)
                            + 0.12 * std::sin(b + 1.10);
                   })
    };
    const auto x2
    {
        zip(phase_a, phase_b)
            >> lift<pair_wire, double>(
                   [](const double& a, const double& b)
                   {
                       return 0.5 + 0.31 * std::sin(a + phase_offset)
                            + 0.12 * std::sin(b + 3.0 * phase_offset + 0.65);
                   })
    };
    const auto y2
    {
        zip(phase_a, phase_b)
            >> lift<pair_wire, double>(
                   [](const double& a, const double& b)
                   {
                       return 0.5 + 0.31 * std::cos(2.0 * (a + phase_offset) + 0.35)
                            + 0.12 * std::sin(b + 3.0 * phase_offset + 1.10);
                   })
    };

    return parallel(
        x >> output<double, clock_tag::ui>("x"),
        y >> output<double, clock_tag::ui>("y"),
        x2 >> output<double, clock_tag::ui>("x2"),
        y2 >> output<double, clock_tag::ui>("y2"),
        phase_a >> output<double, clock_tag::ui>("phase"));
}

class DeterministicReplay
{
public:
    DeterministicReplay()
    {
        reset();
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        if (!std::isfinite(dt) || dt < 0.0)
            dt = 0.0;
        else if (dt > 0.05)
            dt = 0.05;

        m_program->set("dt", dt);
        m_engine.step(*m_program, clock_tag::ui);
    }

    void
    reset()
    {
        m_program.emplace(deterministic_replay_recipe());
        m_program->set("dt", 1.0 / 60.0);
        m_engine.step(*m_program, clock_tag::ui);
    }

    double
    x() const
    {
        return lab::read_double(*m_program, "x");
    }

    double
    y() const
    {
        return lab::read_double(*m_program, "y");
    }

    double
    x2() const
    {
        return lab::read_double(*m_program, "x2");
    }

    double
    y2() const
    {
        return lab::read_double(*m_program, "y2");
    }

    double
    phase() const
    {
        return lab::read_double(*m_program, "phase");
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(deterministic_replay)
{
    emscripten::class_<DeterministicReplay>("DeterministicReplay")
        .constructor()
        .function("tick", &DeterministicReplay::tick)
        .function("reset", &DeterministicReplay::reset)
        .function("x", &DeterministicReplay::x)
        .function("y", &DeterministicReplay::y)
        .function("x2", &DeterministicReplay::x2)
        .function("y2", &DeterministicReplay::y2)
        .function("phase", &DeterministicReplay::phase);
}
#pragma clang diagnostic pop
