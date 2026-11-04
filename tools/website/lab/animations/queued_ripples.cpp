#include "lab_common.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <emscripten/bind.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace
{

using namespace clockr;

using kernel::clock_tag;

auto
queued_ripples_recipe()
{
    using namespace clockr::shell;

    const auto event_x
    {
        at<clock_tag::worker, conc::policy::queued>(
            input<double, clock_tag::ui>("event_x"))
    };
    const auto event_y
    {
        at<clock_tag::worker, conc::policy::queued>(
            input<double, clock_tag::ui>("event_y"))
    };
    const auto pointer_x
    {
        at<clock_tag::worker, conc::policy::latest>(
            input<double, clock_tag::ui>("pointer_x"))
    };
    const auto pointer_y
    {
        at<clock_tag::worker, conc::policy::latest>(
            input<double, clock_tag::ui>("pointer_y"))
    };

    return parallel(
        event_x >> output<double, clock_tag::worker>("drained_x"),
        event_y >> output<double, clock_tag::worker>("drained_y"),
        pointer_x >> output<double, clock_tag::worker>("latest_x"),
        pointer_y >> output<double, clock_tag::worker>("latest_y"));
}

double
unit(double value)
{
    if (!std::isfinite(value))
        return 0.5;

    return std::clamp(value, 0.0, 1.0);
}

class QueuedRipples
{
public:
    QueuedRipples()
    {
        reset();
    }

    void
    push(double x, double y)
    {
        m_pending.emplace_back(unit(x), unit(y));
        ++m_pushed;
    }

    void
    setPointer(double x, double y)
    {
        m_pointer_x = unit(x);
        m_pointer_y = unit(y);
        m_pointer_dirty = true;
    }

    void
    tick(double)
    {
        if (!m_program)
            return;

        const bool had_pending { !m_pending.empty() };

        if (m_pointer_dirty)
        {
            m_program->set("pointer_x", m_pointer_x);
            m_program->set("pointer_y", m_pointer_y);
        }

        for (const auto& [x, y] : m_pending)
        {
            m_program->set("event_x", x);
            m_program->set("event_y", y);
            m_engine.step(*m_program, clock_tag::ui);
        }

        if (!had_pending && m_pointer_dirty)
            m_engine.step(*m_program, clock_tag::ui);

        m_pending.clear();
        m_pointer_dirty = false;

        m_engine.step(*m_program, clock_tag::worker);

        const double event_x { lab::read_double(*m_program, "drained_x") };

        if (!std::isnan(event_x))
        {
            const double event_y { lab::read_double(*m_program, "drained_y") };

            m_event_x = event_x;
            m_event_y = std::isnan(event_y) ? event_x : event_y;
            ++m_drained;
        }
    }

    void
    reset()
    {
        m_program.emplace(queued_ripples_recipe());

        m_pending.clear();
        m_pushed = 0;
        m_drained = 0;
        m_event_x = 0.5;
        m_event_y = 0.5;
        m_pointer_x = 0.5;
        m_pointer_y = 0.5;
        m_pointer_dirty = false;

        m_program->set("event_x", 0.5);
        m_program->set("event_y", 0.5);
        m_program->set("pointer_x", 0.5);
        m_program->set("pointer_y", 0.5);

        m_engine.step(*m_program, clock_tag::ui);
        m_engine.step(*m_program, clock_tag::worker);
    }

    double
    eventX() const
    {
        return m_event_x;
    }

    double
    eventY() const
    {
        return m_event_y;
    }

    double
    latestX() const
    {
        return lab::read_double(*m_program, "latest_x");
    }

    double
    latestY() const
    {
        return lab::read_double(*m_program, "latest_y");
    }

    double
    count() const
    {
        return static_cast<double>(m_pushed);
    }

    double
    drained() const
    {
        return static_cast<double>(m_drained);
    }

    double
    queueDepth() const
    {
        return static_cast<double>(m_pushed > m_drained ? m_pushed - m_drained : 0);
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;

    std::vector<std::pair<double, double>> m_pending;

    double m_event_x { 0.5 };
    double m_event_y { 0.5 };
    double m_pointer_x { 0.5 };
    double m_pointer_y { 0.5 };

    std::uint64_t m_pushed { 0 };
    std::uint64_t m_drained { 0 };

    bool m_pointer_dirty { false };
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(queued_ripples)
{
    emscripten::class_<QueuedRipples>("QueuedRipples")
        .constructor()
        .function("push", &QueuedRipples::push)
        .function("setPointer", &QueuedRipples::setPointer)
        .function("tick", &QueuedRipples::tick)
        .function("reset", &QueuedRipples::reset)
        .function("eventX", &QueuedRipples::eventX)
        .function("eventY", &QueuedRipples::eventY)
        .function("latestX", &QueuedRipples::latestX)
        .function("latestY", &QueuedRipples::latestY)
        .function("count", &QueuedRipples::count)
        .function("drained", &QueuedRipples::drained)
        .function("queueDepth", &QueuedRipples::queueDepth);
}
#pragma clang diagnostic pop
