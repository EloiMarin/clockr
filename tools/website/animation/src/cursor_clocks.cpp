// Cursor Clocks — Clock Loom bent into three concentric clock faces pinned
// under the pointer: an inner spinner on the audio clock (×3), a middle hand
// on ui (×1) and an outer dial on worker (÷4). The crossings are the Loom's:
//
//   latest  audio → ui:     the middle face echoes the spinner's last phase.
//   latest  ui → worker:    the outer face echoes the hand in visible steps.
//   queued  audio → ui → worker: a click is one *value* that hops ring to
//                           ring, scheduled at each crossing: the middle hand
//                           picks it up on the next ui tick, the outer dial on
//                           the next worker tick.
//
// The hop chain is two queued crossings with a host-tick relay in between
// (`hopU` read on the ui seam is set on `relay` for the next ui epoch). A
// purely in-recipe chain would flood: a crossing source that is not an input
// port publishes on every epoch, so the worker's queue would drown in
// empty-drain monostates. The optional (fire-and-forget) inputs keep each
// hop to exactly one epoch per click — the "containment" pattern used by
// Knock & Stamp for the same reason.
//
// The dial's centre is not part of the network at all: the renderer pins the
// rings exactly under the cursor, so there is no position state to carry.

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <emscripten/bind.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <variant>

namespace
{

using namespace clockr;

using kernel::clock_tag;
using clockr::conc::policy;

// Tolerant getter for the renderer: a value that has not been produced yet
// (or is not a number) reads as NaN, matching the lab sketch helpers.
double
read_double(const kernel::program& program, const char* name)
{
    const kernel::value value { program.get(name) };

    if (const auto* d { std::get_if<double>(&value) })
        return *d;

    if (const auto* i { std::get_if<std::int64_t>(&value) })
        return static_cast<double>(*i);

    return std::numeric_limits<double>::quiet_NaN();
}

// Slower than Clock Loom on purpose (tuned for a 60 Hz render loop): audio
// sweeps in ~3.2 s, ui in ~7 s, worker in ~12.5 s. The gentler tempo suits a
// page-wide companion, and it makes the per-ring hand-off waits legible.
constexpr double audio_rate { 0.105 };
constexpr double ui_rate { 0.145 };
constexpr double worker_rate { 0.320 };

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
cursor_clocks_recipe()
{
    using namespace clockr::shell;

    // As in Clock Loom: audio and ui fan out (marker plus a crossing), so
    // they are shared; the worker phase only feeds its own marker.
    const auto audio_phase { share(phase_chain<clock_tag::audio>("dt_audio")) };
    const auto ui_phase { share(phase_chain<clock_tag::ui>("dt_ui")) };
    const auto worker_phase { phase_chain<clock_tag::worker>("dt_worker") };

    const auto ui_sample { at<clock_tag::ui, policy::latest>(audio_phase) };
    const auto worker_sample { at<clock_tag::worker, policy::latest>(ui_phase) };

    // The click value hops inward to outward: fire-and-forget on the inner
    // clock, queued across to the middle one, relayed at the ui seam, queued
    // across to the outer one. An empty drain stages monostate, which the
    // tolerant getter reports as NaN — the renderer's "nothing hopped".
    const auto hop_in
    {
        at<clock_tag::ui, policy::queued>(
            input<std::optional<double>, clock_tag::audio>("click"))
        >> output<std::optional<double>, clock_tag::ui>("hopU")
    };

    const auto hop_out
    {
        at<clock_tag::worker, policy::queued>(
            input<std::optional<double>, clock_tag::ui>("relay"))
        >> output<std::optional<double>, clock_tag::worker>("hopW")
    };

    return parallel(
        audio_phase >> output<double, clock_tag::audio>("aPhase"),
        ui_phase >> output<double, clock_tag::ui>("uPhase"),
        worker_phase >> output<double, clock_tag::worker>("wPhase"),
        ui_sample >> output<double, clock_tag::ui>("uSample"),
        worker_sample >> output<double, clock_tag::worker>("wSample"),
        hop_in,
        hop_out);
}

class CursorClocks
{
public:
    CursorClocks()
    {
        reset();
    }

    void
    push()
    {
        ++m_pending;
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        const double step { std::isfinite(dt) ? std::clamp(dt, 0.0, 0.05) : 1.0 / 60.0 };

        // audio runs three times per rendered frame. At most one click value
        // is published per frame, on the first audio epoch; the fire-and-
        // forget input reverts before the remaining two.
        m_program->set("dt_audio", step * audio_rate);

        if (m_pending > 0)
        {
            m_program->set("click", static_cast<double>(++m_click));
            --m_pending;
        }

        for (int i { 0 }; i < audio_ticks; ++i)
            m_engine.step(*m_program, clock_tag::audio);

        // ui runs once per frame: it samples the audio latest link and
        // drains one hopped value off the inner queue.
        m_program->set("dt_ui", step * ui_rate);
        m_engine.step(*m_program, clock_tag::ui);

        // The relay: anything that just arrived on the middle ring is set on
        // the fire-and-forget relay input, so the next ui epoch hands it to
        // the outer queue. One extra frame of latency by design (see the
        // header comment).
        const kernel::value hop { m_program->get("hopU") };

        if (const auto* arrived { std::get_if<double>(&hop) })
            m_program->set("relay", *arrived);

        // worker runs every fourth frame, sampling ui latest and draining
        // one hopped value off the outer queue.
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
        m_program.emplace(cursor_clocks_recipe());

        m_frame = 0;
        m_click = 0;
        m_pending = 0;

        constexpr double dt { 1.0 / 60.0 };

        // Warm-up: publish audio before ui samples it, then ui before worker
        // samples it, so the two latest crossings have real values on frame
        // zero. The hop channels stay empty: NaN means "no event".
        m_program->set("dt_audio", dt * audio_rate);

        for (int i { 0 }; i < audio_ticks; ++i)
            m_engine.step(*m_program, clock_tag::audio);

        m_program->set("dt_ui", dt * ui_rate);
        m_engine.step(*m_program, clock_tag::ui);

        m_program->set("dt_worker", dt * worker_rate);
        m_engine.step(*m_program, clock_tag::worker);
    }

    double
    aPhase() const
    {
        return read_double(*m_program, "aPhase");
    }

    double
    uPhase() const
    {
        return read_double(*m_program, "uPhase");
    }

    double
    wPhase() const
    {
        return read_double(*m_program, "wPhase");
    }

    double
    uSample() const
    {
        return read_double(*m_program, "uSample");
    }

    double
    wSample() const
    {
        return read_double(*m_program, "wSample");
    }

    double
    hopU() const
    {
        return read_double(*m_program, "hopU");
    }

    double
    hopW() const
    {
        return read_double(*m_program, "hopW");
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;

    std::uint64_t m_frame { 0 };
    std::uint64_t m_click { 0 };
    std::uint64_t m_pending { 0 };
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(clockr_page_cursor_clocks)
{
    emscripten::class_<CursorClocks>("CursorClocks")
        .constructor()
        .function("push", &CursorClocks::push)
        .function("tick", &CursorClocks::tick)
        .function("reset", &CursorClocks::reset)
        .function("aPhase", &CursorClocks::aPhase)
        .function("uPhase", &CursorClocks::uPhase)
        .function("wPhase", &CursorClocks::wPhase)
        .function("uSample", &CursorClocks::uSample)
        .function("wSample", &CursorClocks::wSample)
        .function("hopU", &CursorClocks::hopU)
        .function("hopW", &CursorClocks::hopW);
}
#pragma clang diagnostic pop
