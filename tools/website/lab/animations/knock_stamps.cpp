// Knock & Stamp — click to pulse the orbit: the knock speeds it up (and widens
// its figure), and the same click stamps a ghost of exactly where the orbit
// was. The stamp is not approximated after the fact: the event and the orb
// position meet inside one ui tick, so the held point is the position the live
// orb had on that very step (law L1).
//
// ui clock: an optional pulse input — disengaged except on the tick the host
// sets it — is read in three places: a decaying knock scan, the phase speed,
// and one three-input fold that adopts `{x, y, count}` on the firing tick and
// holds it otherwise (stepper generalized over the stamped point).

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

// All three fit the inline user-value budget.
struct knock_in
{
    float dt { 0.0f };
    bool fired { false };
};

struct stamp_in
{
    float x { 0.5f };
    float y { 0.5f };
    bool fired { false };
};

struct snap_state
{
    float x { 0.5f };
    float y { 0.5f };
    float count { 0.0f };
};

constexpr double tau { 6.283185307179586 };

double
wrap01(double value)
{
    value -= std::floor(value);
    return value;
}

auto
knock_stamps_recipe()
{
    using namespace clockr::shell;

    const auto dt { share(input<double, clock_tag::ui>("dt")) };
    const auto pulse { share(input<std::optional<double>, clock_tag::ui>("pulse")) };

    // The knock: an impulse on the firing tick, decaying everywhere else —
    // the host reads it to flash the halo, the phase reads it to speed up.
    const auto knock
    {
        share(
            zip(pulse, dt)
                >> lift<std::pair<std::optional<double>, double>, knock_in>(
                       [](const std::optional<double>& ev, const double& d)
                       {
                           return knock_in { static_cast<float>(d), ev.has_value() };
                       })
                >> scan<knock_in, double>(
                       0.0,
                       [](const double& s, const knock_in& in)
                       {
                           const double decay { std::exp(-3.3 * in.dt) };

                           return s * decay + (in.fired ? 1.0 : 0.0);
                       }))
    };

    // The orbit: each knock widens the figure; a struck orbit turns faster
    // for a moment, so repeated clicks visibly wind it up.
    const auto speed
    {
        zip(dt, knock)
            >> lift<std::pair<double, double>, double>(
                   [](const double& d, const double& k)
                   { return d * (0.10 + 0.55 * k); })
    };
    const auto phase
    {
        share(
            speed
                >> scan<double, double>(
                       0.0,
                       [](const double& s, const double& d) { return wrap01(s + d); }))
    };

    const auto orb_x
    {
        zip(phase, knock)
            >> lift<std::pair<double, double>, double>(
                   [](const double& p, const double& k)
                   {
                       const double r { 0.30 + 0.08 * k };

                       return 0.5 + r * std::sin(tau * p);
                   })
    };
    const auto orb_y
    {
        zip(phase, knock)
            >> lift<std::pair<double, double>, double>(
                   [](const double& p, const double& k)
                   {
                       const double r { 0.24 + 0.06 * k };

                       return 0.5 + r * std::sin(tau * 1.6 * p + 0.6);
                   })
    };

    // The stamp: the firing tick adopts the orb's position on that same step.
    // Silent ticks keep the last stamp; `count` grows by one per firing.
    // Shared once: three outputs read the same single scan.
    const auto stamp
    {
        share(
            zip(pulse, orb_x, orb_y)
                >> lift<std::pair<std::pair<std::optional<double>, double>, double>, stamp_in>(
                       [](const std::optional<double>& ev, const double& x, const double& y)
                       {
                           return stamp_in
                           {
                               static_cast<float>(x),
                               static_cast<float>(y),
                               ev.has_value()
                           };
                       })
                >> scan<stamp_in, snap_state>(
                       snap_state { 0.5f, 0.5f, 0.0f },
                       [](const snap_state& s, const stamp_in& in)
                       {
                           if (in.fired)
                               return snap_state { in.x, in.y, s.count + 1.0f };

                           return s;
                       }))
    };

    return parallel(
        orb_x >> output<double, clock_tag::ui>("orbX"),
        orb_y >> output<double, clock_tag::ui>("orbY"),
        knock >> output<double, clock_tag::ui>("knock"),
        stamp >> lift<snap_state, double>([](const snap_state& s) { return s.x; })
               >> output<double, clock_tag::ui>("snapX"),
        stamp >> lift<snap_state, double>([](const snap_state& s) { return s.y; })
               >> output<double, clock_tag::ui>("snapY"),
        stamp >> lift<snap_state, double>([](const snap_state& s) { return s.count; })
               >> output<double, clock_tag::ui>("stampCount"));
}

class KnockStamps
{
public:
    KnockStamps()
    {
        reset();
    }

    // Queue one click: it fires on the next ui tick, and successive calls
    // queue further ticks (one event per tick, in order).
    void
    pulse()
    {
        ++m_pending;
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        const double step { std::isfinite(dt) ? std::clamp(dt, 0.0, 0.05) : 1.0 / 60.0 };

        // The event input is engaged for exactly one tick per queued click;
        // every other tick it is disengaged.
        const std::optional<double> fired { m_pending > 0 ? std::optional<double> { 1.0 } : std::nullopt };
        m_program->set("pulse", shell::to_value(fired));

        m_program->set("dt", kernel::value { step });
        m_engine.step(*m_program, clock_tag::ui);

        if (m_pending > 0)
            --m_pending;
    }

    void
    reset()
    {
        m_program.emplace(knock_stamps_recipe());

        m_pending = 0;

        m_program->set("pulse", shell::to_value(std::nullopt));
        m_program->set("dt", kernel::value { 1.0 / 60.0 });

        m_engine.step(*m_program, clock_tag::ui);
    }

    double
    orbX() const
    {
        return lab::read_double(*m_program, "orbX");
    }

    double
    orbY() const
    {
        return lab::read_double(*m_program, "orbY");
    }

    double
    knock() const
    {
        return lab::read_double(*m_program, "knock");
    }

    double
    snapX() const
    {
        return m_program ? lab::read_double(*m_program, "snapX") : std::numeric_limits<double>::quiet_NaN();
    }

    double
    snapY() const
    {
        return m_program ? lab::read_double(*m_program, "snapY") : std::numeric_limits<double>::quiet_NaN();
    }

    double
    stampCount() const
    {
        return m_program ? lab::read_double(*m_program, "stampCount") : std::numeric_limits<double>::quiet_NaN();
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;

    std::uint64_t m_pending { 0 };
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(lab_knock_stamps)
{
    emscripten::class_<KnockStamps>("KnockStamps")
        .constructor()
        .function("pulse", &KnockStamps::pulse)
        .function("tick", &KnockStamps::tick)
        .function("reset", &KnockStamps::reset)
        .function("orbX", &KnockStamps::orbX)
        .function("orbY", &KnockStamps::orbY)
        .function("knock", &KnockStamps::knock)
        .function("snapX", &KnockStamps::snapX)
        .function("snapY", &KnockStamps::snapY)
        .function("stampCount", &KnockStamps::stampCount);
}
#pragma clang diagnostic pop
