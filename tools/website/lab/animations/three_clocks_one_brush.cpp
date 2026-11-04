// Three Clocks, One Brush — three translucent brushes paint one growing
// canvas at three clock rates.
//
// audio:  a fast phase pair, painting smooth, tightly spaced strokes.
// ui:     a medium phase pair, painting a slower and calmer stroke.
// worker: a phase pair sampled onto the ui clock through `latest`, so that
//         brush only advances on worker frames and moves in visible steps.
//
// Coordinates leave the wasm in [0, 1] and never leave the canvas; the
// renderer only maps them to pixels and fades the previous frame.

#include "lab_common.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <emscripten/bind.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace
{

using namespace clockr;

using kernel::clock_tag;
using clockr::conc::policy;

constexpr double tau { 6.283185307179586 };

// Phase advances per tick, chosen for a 60 Hz render loop. The worker's larger
// per-tick value compensates for ticking only every fourth frame; its update
// remains visibly chunked because the crossing only refreshes on worker ticks.
constexpr double audio_rate { 0.140 };
constexpr double ui_rate { 0.150 };
constexpr double worker_rate { 0.720 };

constexpr int audio_ticks { 3 };
constexpr int worker_period { 4 };

double
wrap01(double value)
{
    if (!std::isfinite(value))
        return 0.0;

    value -= std::floor(value);

    return value;
}

double
brush_x(double phase)
{
    return 0.5 + 0.40 * std::sin(tau * phase) + 0.06 * std::sin(tau * (2.3 * phase + 0.11));
}

double
brush_y(double phase)
{
    return 0.5 + 0.40 * std::sin(tau * (phase + 0.23)) + 0.06 * std::cos(tau * (1.7 * phase + 0.4));
}

template<typename input_t>
auto
phase_hold(input_t delta, double seed, double rate)
{
    using namespace clockr::shell;

    return delta
        >> scan<double, double>(
               seed,
               [rate](const double& state, const double& d)
               { return wrap01(state + d * rate); });
}

auto
three_clocks_one_brush_recipe()
{
    using namespace clockr::shell;

    // audio pair: two holds on one shared dt input.
    const auto audio_dt { share(input<double, clock_tag::audio>("dt_audio")) };
    const auto audio_x { phase_hold(audio_dt, 0.00, 1.00) };
    const auto audio_y { phase_hold(audio_dt, 0.37, 1.37) };

    // worker pair: both phases are sampled onto ui with `latest`, then mapped
    // to coordinates there. The crossing therefore refreshes only when the
    // worker ticks (every fourth frame), which is the visible step.
    const auto worker_dt { share(input<double, clock_tag::worker>("dt_worker")) };
    const auto worker_x { phase_hold(worker_dt, 0.81, 1.00) };
    const auto worker_y { phase_hold(worker_dt, 0.63, 1.71) };

    const auto worker_x_ui
    {
        at<clock_tag::ui, policy::latest>(worker_x)
        >> lift<double, double>([](const double& p) { return brush_x(p); })
    };
    const auto worker_y_ui
    {
        at<clock_tag::ui, policy::latest>(worker_y)
        >> lift<double, double>([](const double& p) { return brush_y(p); })
    };

    // ui pair: the calm, fully smooth brush.
    const auto ui_dt { share(input<double, clock_tag::ui>("dt_ui")) };
    const auto ui_x
    {
        phase_hold(ui_dt, 0.52, 1.00)
        >> lift<double, double>([](const double& p) { return brush_x(p); })
    };
    const auto ui_y
    {
        phase_hold(ui_dt, 0.11, 0.83)
        >> lift<double, double>([](const double& p) { return brush_y(p); })
    };

    return parallel(
        (audio_x >> lift<double, double>([](const double& p) { return brush_x(p); }))
            >> output<double, clock_tag::audio>("x0"),
        (audio_y >> lift<double, double>([](const double& p) { return brush_y(p); }))
            >> output<double, clock_tag::audio>("y0"),
        worker_x_ui >> output<double, clock_tag::ui>("x1"),
        worker_y_ui >> output<double, clock_tag::ui>("y1"),
        ui_x >> output<double, clock_tag::ui>("x2"),
        ui_y >> output<double, clock_tag::ui>("y2"));
}

class ThreeClocksOneBrush
{
public:
    ThreeClocksOneBrush()
    {
        reset();
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        const double step { std::isfinite(dt) ? std::clamp(dt, 0.0, 0.05) : 1.0 / 60.0 };

        m_program->set("dt_audio", step * audio_rate);

        for (int i { 0 }; i < audio_ticks; ++i)
            m_engine.step(*m_program, clock_tag::audio);

        // Publish the worker's fresh phases before the ui clock samples them,
        // so the stepped brush is current on the frame it advances.
        if ((m_frame % worker_period) == 0)
        {
            m_program->set("dt_worker", step * worker_rate);
            m_engine.step(*m_program, clock_tag::worker);
        }

        m_program->set("dt_ui", step * ui_rate);
        m_engine.step(*m_program, clock_tag::ui);

        ++m_frame;
    }

    void
    reset()
    {
        m_program.emplace(three_clocks_one_brush_recipe());
        m_frame = 0;

        constexpr double dt { 1.0 / 60.0 };

        // Warm-up: audio first, then the worker, then the ui clock that samples
        // the worker through its latest crossings. Every getter is finite on
        // frame zero.
        m_program->set("dt_audio", dt * audio_rate);

        for (int i { 0 }; i < audio_ticks; ++i)
            m_engine.step(*m_program, clock_tag::audio);

        m_program->set("dt_worker", dt * worker_rate);
        m_engine.step(*m_program, clock_tag::worker);

        m_program->set("dt_ui", dt * ui_rate);
        m_engine.step(*m_program, clock_tag::ui);
    }

    double
    x0() const
    {
        return lab::read_double(*m_program, "x0");
    }

    double
    y0() const
    {
        return lab::read_double(*m_program, "y0");
    }

    double
    x1() const
    {
        return lab::read_double(*m_program, "x1");
    }

    double
    y1() const
    {
        return lab::read_double(*m_program, "y1");
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

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;

    std::uint64_t m_frame { 0 };
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(lab_three_clocks_one_brush)
{
    emscripten::class_<ThreeClocksOneBrush>("ThreeClocksOneBrush")
        .constructor()
        .function("tick", &ThreeClocksOneBrush::tick)
        .function("reset", &ThreeClocksOneBrush::reset)
        .function("x0", &ThreeClocksOneBrush::x0)
        .function("y0", &ThreeClocksOneBrush::y0)
        .function("x1", &ThreeClocksOneBrush::x1)
        .function("y1", &ThreeClocksOneBrush::y1)
        .function("x2", &ThreeClocksOneBrush::x2)
        .function("y2", &ThreeClocksOneBrush::y2);
}
#pragma clang diagnostic pop
