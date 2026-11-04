// Signal Bloom — a harmonograph ribbon whose phase, feedback strand and
// cross-clock ghost all live in a resonant recipe.
//
// ui clock:    dt + pointer -> modulated speed -> two phase holds -> head
//              position; a chain of delays gives the trailing feedback strand.
// worker clock: samples the head through a `latest` crossing every fourth
//              frame, so the ghost holds and then steps.
//
// The shell is deliberately tiny: set the pointer, tick, read a handful of
// scalars. All the motion is the recipe's state.

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
using clockr::conc::policy;

using pair_wire = std::pair<double, double>;
using head_wire = std::pair<std::pair<double, double>, double>;

auto
signal_bloom_recipe()
{
    using namespace clockr::shell;

    const auto dt { share(input<double, clock_tag::ui>("dt")) };
    const auto px { share(input<double, clock_tag::ui>("px")) };
    const auto py { share(input<double, clock_tag::ui>("py")) };

    // The pointer bends the rate at which each phase advances: slow, breathing
    // motion on one axis and a faster, more eccentric one on the other.
    const auto speed_a
    {
        zip(dt, py)
            >> lift<pair_wire, double>(
                   [](const double& d, const double& p)
                   { return d * (0.85 + 0.65 * p); })
    };
    const auto speed_b
    {
        zip(dt, px)
            >> lift<pair_wire, double>(
                   [](const double& d, const double& p)
                   { return d * (1.25 + 0.85 * p); })
    };

    const auto phase_a
    {
        share(
            speed_a
                >> scan<double, double>(
                       0.0,
                       [](const double& s, const double& d) { return s + d; }))
    };
    const auto phase_b
    {
        share(
            speed_b
                >> scan<double, double>(
                       0.4,
                       [](const double& s, const double& d) { return s + d; }))
    };

    // A two-term harmonograph per axis. The pointer also scales the amplitude,
    // so moving it opens and closes the bloom.
    const auto head_x
    {
        share(
            zip(phase_a, phase_b, px)
                >> lift<head_wire, double>(
                       [](const double& a, const double& b, const double& p)
                       {
                           return (0.60 * std::sin(a) + 0.24 * std::sin(b * 1.7 + 1.3))
                                  * (0.72 + 0.38 * p);
                       }))
    };
    const auto head_y
    {
        share(
            zip(phase_a, phase_b, py)
                >> lift<head_wire, double>(
                       [](const double& a, const double& b, const double& p)
                       {
                           return (0.60 * std::sin(b + 0.4) + 0.24 * std::cos(a * 0.9))
                                  * (0.72 + 0.38 * p);
                       }))
    };

    // Eight unit delays: the feedback strand is the head from a moment ago, so
    // it visibly trails the bright point instead of sitting on top of it.
    const auto feed_x
    {
        head_x
            >> delay<double>(0.0) >> delay<double>(0.0) >> delay<double>(0.0) >> delay<double>(0.0)
            >> delay<double>(0.0) >> delay<double>(0.0) >> delay<double>(0.0) >> delay<double>(0.0)
    };
    const auto feed_y
    {
        head_y
            >> delay<double>(0.0) >> delay<double>(0.0) >> delay<double>(0.0) >> delay<double>(0.0)
            >> delay<double>(0.0) >> delay<double>(0.0) >> delay<double>(0.0) >> delay<double>(0.0)
    };

    return parallel(
        head_x >> output<double, clock_tag::ui>("headX"),
        head_y >> output<double, clock_tag::ui>("headY"),
        feed_x >> output<double, clock_tag::ui>("feedX"),
        feed_y >> output<double, clock_tag::ui>("feedY"),
        phase_a >> output<double, clock_tag::ui>("phase"),
        at<clock_tag::worker, policy::latest>(head_x)
            >> output<double, clock_tag::worker>("ghostX"),
        at<clock_tag::worker, policy::latest>(head_y)
            >> output<double, clock_tag::worker>("ghostY"));
}

class SignalBloom
{
public:
    SignalBloom()
    {
        reset();
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        const double step { std::isfinite(dt) ? std::clamp(dt, 0.0, 0.05) : 1.0 / 60.0 };
        const double blend { 1.0 - std::exp(-step * 3.5) };

        m_px += (m_target_x - m_px) * blend;
        m_py += (m_target_y - m_py) * blend;

        m_program->set("dt", step);
        m_program->set("px", m_px);
        m_program->set("py", m_py);

        m_engine.step(*m_program, clock_tag::ui);

        if ((++m_frame % 4) == 0)
            m_engine.step(*m_program, clock_tag::worker);
    }

    void
    setPointer(double x, double y)
    {
        m_target_x = std::clamp(std::isfinite(x) ? x : 0.5, 0.0, 1.0);
        m_target_y = std::clamp(std::isfinite(y) ? y : 0.5, 0.0, 1.0);
    }

    void
    reset()
    {
        m_program.emplace(signal_bloom_recipe());

        m_px = 0.5;
        m_py = 0.5;
        m_target_x = 0.5;
        m_target_y = 0.5;
        m_frame = 0;

        m_program->set("dt", 1.0 / 60.0);
        m_program->set("px", m_px);
        m_program->set("py", m_py);

        // Let both phases and the delay chain fill so the first frame already
        // draws a curve rather than a dot.
        for (int i { 0 }; i < 90; ++i)
            m_engine.step(*m_program, clock_tag::ui);

        m_engine.step(*m_program, clock_tag::worker);
    }

    double
    headX() const
    {
        return lab::read_double(*m_program, "headX");
    }

    double
    headY() const
    {
        return lab::read_double(*m_program, "headY");
    }

    double
    feedX() const
    {
        return lab::read_double(*m_program, "feedX");
    }

    double
    feedY() const
    {
        return lab::read_double(*m_program, "feedY");
    }

    double
    ghostX() const
    {
        return lab::read_double(*m_program, "ghostX");
    }

    double
    ghostY() const
    {
        return lab::read_double(*m_program, "ghostY");
    }

    double
    phase() const
    {
        return lab::read_double(*m_program, "phase");
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;

    double m_px { 0.5 };
    double m_py { 0.5 };
    double m_target_x { 0.5 };
    double m_target_y { 0.5 };
    int m_frame { 0 };
};

} // namespace

EMSCRIPTEN_BINDINGS(lab_signal_bloom)
{
    emscripten::class_<SignalBloom>("SignalBloom")
        .constructor()
        .function("tick", &SignalBloom::tick)
        .function("setPointer", &SignalBloom::setPointer)
        .function("reset", &SignalBloom::reset)
        .function("headX", &SignalBloom::headX)
        .function("headY", &SignalBloom::headY)
        .function("feedX", &SignalBloom::feedX)
        .function("feedY", &SignalBloom::feedY)
        .function("ghostX", &SignalBloom::ghostX)
        .function("ghostY", &SignalBloom::ghostY)
        .function("phase", &SignalBloom::phase);
}
