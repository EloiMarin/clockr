// Fling — grab the ball with the pointer and throw it.
//
// ui clock: the target (pointer hover, or the pointer itself while grabbed)
// and a grabbed flag feed two scans, one per axis. Each scan's whole state is
// a small inline user struct (position + velocity): while grabbed it is a
// tight critically damped spring; released, the same scan loosens, so the
// velocity you leave it with becomes the throw, the walls bounce it, and the
// slack spring slowly reels it back toward the target. JS only draws.

#include "ball_hold.hpp"
#include "lab_common.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <emscripten/bind.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace
{

using namespace clockr;

using kernel::clock_tag;
using lab_ball::ball_in;
using lab_ball::ball_state;
using lab_ball::ball_chain;

auto
fling_recipe()
{
    using namespace clockr::shell;

    const auto target_x { share(input<double, clock_tag::ui>("targetX")) };
    const auto target_y { share(input<double, clock_tag::ui>("targetY")) };
    const auto dt { share(input<double, clock_tag::ui>("dt")) };
    const auto grabbed { share(input<bool, clock_tag::ui>("grabbed")) };

    const auto ball_x { ball_chain(target_x, dt, grabbed) };
    const auto ball_y { ball_chain(target_y, dt, grabbed) };

    // The scan's whole state travels out as one inline user value; the class
    // decodes position and velocity from it.
    return parallel(
        ball_x >> output<ball_state, clock_tag::ui>("ballX"),
        ball_y >> output<ball_state, clock_tag::ui>("ballY"));
}

double
fetch_double(const kernel::value& value)
{
    if (const auto* d { std::get_if<double>(&value) })
        return *d;

    return std::numeric_limits<double>::quiet_NaN();
}

double
fetch_state(const kernel::value& value, bool want_velocity)
{
    const auto* in { std::get_if<kernel::user_inline>(&value) };

    if (!in || in->type != &kernel::tag_of<ball_state>())
        return std::numeric_limits<double>::quiet_NaN();

    const auto& s { *reinterpret_cast<const ball_state*>(in->bytes.data()) };

    return want_velocity ? s.v : s.x;
}

class Fling
{
public:
    Fling()
    {
        reset();
    }

    void
    setPointer(double x, double y, bool down)
    {
        if (!m_program)
            return;

        const double nx { std::clamp(std::isfinite(x) ? x : 0.5, 0.0, 1.0) };
        const double ny { std::clamp(std::isfinite(y) ? y : 0.5, 0.0, 1.0) };

        m_program->set("targetX", kernel::value { nx });
        m_program->set("targetY", kernel::value { ny });

        if (m_down != down)
        {
            m_down = down;
            m_program->set("grabbed", kernel::value { down });
        }
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        const double step { std::isfinite(dt) ? std::clamp(dt, 0.0, 0.05) : 1.0 / 60.0 };

        m_program->set("dt", kernel::value { step });
        m_engine.step(*m_program, clock_tag::ui);
    }

    void
    reset()
    {
        m_program.emplace(fling_recipe());

        m_down = false;

        m_program->set("targetX", kernel::value { 0.5 });
        m_program->set("targetY", kernel::value { 0.5 });
        m_program->set("dt", kernel::value { 1.0 / 60.0 });
        m_program->set("grabbed", kernel::value { false });

        m_engine.step(*m_program, clock_tag::ui);
    }

    double
    ballX() const
    {
        return m_program ? fetch_state(m_program->get("ballX"), false) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    ballY() const
    {
        return m_program ? fetch_state(m_program->get("ballY"), false) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    velX() const
    {
        return m_program ? fetch_state(m_program->get("ballX"), true) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    velY() const
    {
        return m_program ? fetch_state(m_program->get("ballY"), true) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    targetX() const
    {
        return m_program ? fetch_double(m_program->get_input("targetX")) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    targetY() const
    {
        return m_program ? fetch_double(m_program->get_input("targetY")) : std::numeric_limits<double>::quiet_NaN();
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;

    bool m_down { false };
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(lab_fling)
{
    emscripten::class_<Fling>("Fling")
        .constructor()
        .function("tick", &Fling::tick)
        .function("setPointer", &Fling::setPointer)
        .function("reset", &Fling::reset)
        .function("ballX", &Fling::ballX)
        .function("ballY", &Fling::ballY)
        .function("velX", &Fling::velX)
        .function("velY", &Fling::velY)
        .function("targetX", &Fling::targetX)
        .function("targetY", &Fling::targetY);
}
#pragma clang diagnostic pop
