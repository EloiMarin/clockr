// Clock Loom — three clock regions ticking at different rates, wired by one
// `latest` crossing per hand-off and one `queued` channel for discrete pulses.
//
// audio:  a phase scan advanced three times per frame.
// ui:     its own phase scan, plus the audio phase sampled through `latest`.
// worker: its own phase scan, plus the ui phase sampled through `latest`, and
//         a queued pulse counter drained one event per worker tick.
//
// The visual is entirely driven by phases in [0, 1): the shell exposes the
// three lane phases, the two sampled phases, and the most recent queued pulse.
// A not-yet-delivered pulse reads as NaN, which is exactly the "no event this
// tick" signal the renderer uses to spawn a bead.

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

// Per-tick phase advances are tuned for a 60 Hz render loop: audio sweeps in
// ~2.5 s, ui in ~8 s, worker in ~14 s. The worker only ticks every fourth
// frame, so its per-tick advance is the largest.
constexpr double audio_rate { 0.139 };
constexpr double ui_rate { 0.200 };
constexpr double worker_rate { 0.444 };

constexpr int audio_ticks { 3 };
constexpr int worker_period { 4 };
constexpr int pulse_period { 6 };

double
wrap01(double value)
{
    if (!std::isfinite(value))
        return 0.0;

    value -= std::floor(value);

    return value;
}

template<clock_tag Tag>
auto
phase_chain(std::string_view port)
{
    using namespace clockr::shell;

    return input<double, Tag>(port)
        >> scan<double, double>(
              0.0,
              [](const double& state, const double& delta)
              { return wrap01(state + delta); });
}

auto
clock_loom_recipe()
{
    using namespace clockr::shell;

    // Each lane's own phase. Audio and ui fan out (their marker plus a
    // crossing), so they are shared once; the worker phase only feeds its
    // output.
    const auto audio_phase { share(phase_chain<clock_tag::audio>("dt_audio")) };
    const auto ui_phase { share(phase_chain<clock_tag::ui>("dt_ui")) };
    const auto worker_phase { phase_chain<clock_tag::worker>("dt_worker") };

    // The latest crossings: the consumer lane always sees the producer's most
    // recent phase, so the fibre is continuous (with a small lag) rather than
    // event-driven.
    const auto ui_sample { at<clock_tag::ui, policy::latest>(audio_phase) };
    const auto worker_sample { at<clock_tag::worker, policy::latest>(ui_phase) };

    // The queued crossing: one event per ui tick on which the pulse input was
    // set, drained one per worker tick. An empty drain stages monostate, which
    // the tolerant getter reports as NaN.
    const auto pulse
    {
        at<clock_tag::worker, policy::queued>(
            input<double, clock_tag::ui>("pulse"))
        >> output<double, clock_tag::worker>("pulse")
    };

    return parallel(
        audio_phase >> output<double, clock_tag::audio>("aPhase"),
        ui_phase >> output<double, clock_tag::ui>("uPhase"),
        worker_phase >> output<double, clock_tag::worker>("wPhase"),
        ui_sample >> output<double, clock_tag::ui>("uSample"),
        worker_sample >> output<double, clock_tag::worker>("wSample"),
        pulse);
}

class ClockLoom
{
public:
    ClockLoom()
    {
        reset();
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        const double step { std::isfinite(dt) ? std::clamp(dt, 0.0, 0.05) : 1.0 / 60.0 };

        // audio runs three times per rendered frame.
        m_program->set("dt_audio", step * audio_rate);

        for (int i { 0 }; i < audio_ticks; ++i)
            m_engine.step(*m_program, clock_tag::audio);

        // ui runs once per frame and samples the audio latest link.
        m_program->set("dt_ui", step * ui_rate);

        // A pulse is only set on some epochs; clean epochs publish nothing, so
        // the queued consumer sees genuine gaps between events.
        if ((m_frame % pulse_period) == 0)
            m_program->set("pulse", static_cast<double>(++m_pulse));

        m_engine.step(*m_program, clock_tag::ui);

        // worker runs every fourth frame, sampling ui latest and draining one
        // queued event.
        if ((m_frame % worker_period) == 0)
        {
            m_program->set("dt_worker", step * worker_rate);
            m_engine.step(*m_program, clock_tag::worker);
        }

        ++m_frame;
    }

    void
    reset()
    {
        m_program.emplace(clock_loom_recipe());

        m_frame = 0;
        m_pulse = 0;

        constexpr double dt { 1.0 / 60.0 };

        // Warm-up: publish audio before ui samples it, then ui before worker
        // samples it, and let the worker drain the first queued pulse so every
        // exposed output has a real value on frame zero.
        m_program->set("dt_audio", dt * audio_rate);

        for (int i { 0 }; i < audio_ticks; ++i)
            m_engine.step(*m_program, clock_tag::audio);

        m_program->set("dt_ui", dt * ui_rate);
        m_program->set("pulse", 0.0);
        m_engine.step(*m_program, clock_tag::ui);

        m_program->set("dt_worker", dt * worker_rate);
        m_engine.step(*m_program, clock_tag::worker);
    }

    double
    aPhase() const
    {
        return lab::read_double(*m_program, "aPhase");
    }

    double
    uPhase() const
    {
        return lab::read_double(*m_program, "uPhase");
    }

    double
    wPhase() const
    {
        return lab::read_double(*m_program, "wPhase");
    }

    double
    uSample() const
    {
        return lab::read_double(*m_program, "uSample");
    }

    double
    wSample() const
    {
        return lab::read_double(*m_program, "wSample");
    }

    double
    pulse() const
    {
        return lab::read_double(*m_program, "pulse");
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;

    std::uint64_t m_frame { 0 };
    std::uint64_t m_pulse { 0 };
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(lab_clock_loom)
{
    emscripten::class_<ClockLoom>("ClockLoom")
        .constructor()
        .function("tick", &ClockLoom::tick)
        .function("reset", &ClockLoom::reset)
        .function("aPhase", &ClockLoom::aPhase)
        .function("uPhase", &ClockLoom::uPhase)
        .function("wPhase", &ClockLoom::wPhase)
        .function("uSample", &ClockLoom::uSample)
        .function("wSample", &ClockLoom::wSample)
        .function("pulse", &ClockLoom::pulse);
}
#pragma clang diagnostic pop
