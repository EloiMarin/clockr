// Stirred Field — a slow, faint flow field fills the canvas and the fling
// ball flies above it, stirring it.
//
// ui clock: the ball is the Fling mechanics (shared `ball_hold.hpp`), and a
// `stir` scan folds the ball's speed into a smoothed energy — fling fast and
// the whole field quickens.
//
// worker clock: the field environment (spin/breathe/sway phases and the
// angle/curl/scale/speed parameters they form) advances on its own cadence,
// stepped every third frame with a compensated dt. The stir crosses from ui
// to worker through a `latest` link, and the field parameters cross back the
// same way in the getter path — the render clock samples the environment, it
// does not drive it. The worker also folds a current (curX/curY): a slow push
// on the ball, scaled by the stir, that closes ball -> stir -> current -> ball
// (the return edge rides ui input ports; see the recipe comment). JS draws
// the particles and treats the ball as an obstacle with circulation: flow
// deflects around it and spins with the stir, stronger the faster the ball is
// flying — and the ball drifts with the current.

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
using clockr::conc::policy;
using lab_ball::ball_in;
using lab_ball::ball_state;
using lab_ball::ball_chain;

struct stir_in
{
    float energy { 0.0f };
    float dt { 0.0f };
};

// A three-branch zip wire is left-nested: one leaf per ball axis plus dt.
using stirred_wire_t = std::pair<std::pair<ball_state, ball_state>, double>;

using pair_t = std::pair<double, double>;

// The field's push on the ball, both axes in one 16-byte inline value so the
// recipe keeps `parallel` within its 8-branch limit.
struct field_current
{
    double x { 0.0 };
    double y { 0.0 };
};

// The scene's audio identity: how the field should sound, folded from the
// same phases and stir that steer the visuals. Three floats so the value
// rides inline (16-byte capacity). `speed` lives here rather than in its own
// output for the same 8-branch reason.
struct scene_audio
{
    float pitch { 147.0f };
    float level { 0.04f };
    float speed { 8.0f };
};

// Two of the field's shape parameters in one 16-byte inline value — merges
// their chains so `parallel` keeps room for the spirit tide.
struct field_curve
{
    double angle { 0.0 };
    double curl { 1.05 };
};

// The spirit tide: the plane no current can enter. Driven only by its own
// clock, folded from minutes-scale phases; everything downstream reads it
// through `latest` crossings. Four floats, one 16-byte inline value.
struct spirit_tide
{
    float restlessness { 0.5f };
    float warmth { 0.5f };
    float calm { 0.7f };
    float hue { 0.37f };
};

// A four-branch zip wire is left-nested: phase, phase, phase, stir.
using quad_wire_t = std::pair<std::pair<pair_t, double>, double>;

// Ambient phase at `rate` rad/s (rate is a template constant, so each phase
// gets its own fold type), boosted while the ball is stirring.
template<double rate>
auto
phase_chain(const auto& dt, const auto& stir, double seed)
{
    using namespace clockr::shell;

    return zip(dt, stir)
        >> lift<pair_t, double>(
               [](const double& d, const double& k)
               { return d * (rate * (1.0 + 0.9 * k)); })
        >> scan<double, double>(
               seed,
               [](const double& s, const double& d) { return std::fmod(s + d, 6.283185307179586); });
}

auto
stirred_field_recipe()
{
    using namespace clockr::shell;

    const auto dt { share(input<double, clock_tag::ui>("dt")) };
    const auto target_x { share(input<double, clock_tag::ui>("targetX")) };
    const auto target_y { share(input<double, clock_tag::ui>("targetY")) };
    const auto grabbed { share(input<bool, clock_tag::ui>("grabbed")) };

    // The field's push on the ball, fed back by the host from the worker's
    // current outputs: the loop ball -> stir -> current would be a recipe
    // cycle, which the shell's append-only arena cannot spell yet, so the
    // return edge closes through two ui input ports. Everything else on the
    // loop is engine-side crossings.
    const auto push_x { share(input<double, clock_tag::ui>("pushX")) };
    const auto push_y { share(input<double, clock_tag::ui>("pushY")) };

    // The little being: while free, the host publishes a swim offset on the
    // spring target — autonomous darts that mostly ride along the flow and
    // shy away from a trespassing pointer. Treated as target geometry, so it
    // composes with the hand exactly like the pointer chase does.
    const auto swim_x { share(input<double, clock_tag::ui>("swimX")) };
    const auto swim_y { share(input<double, clock_tag::ui>("swimY")) };

    const auto ball_x { share(ball_chain(target_x, dt, grabbed, push_x, swim_x)) };
    const auto ball_y { share(ball_chain(target_y, dt, grabbed, push_y, swim_y)) };

    // Stir energy: the ball's speed, smoothed. This is what couples the two
    // regions — the field's worker-side scans read a value only the ball
    // (ui region) can raise, crossing the boundary through `latest`.
    const auto stir
    {
        share(
            zip(ball_x, ball_y, dt)
                >> lift<stirred_wire_t, stir_in>(
                       [](const ball_state& sx, const ball_state& sy, const double& d)
                       {
                           return stir_in
                           {
                               static_cast<float>(std::hypot(sx.v, sy.v)),
                               static_cast<float>(d)
                           };
                       })
                >> scan<stir_in, double>(
                       0.0,
                       [](const double& s, const stir_in& in)
                       {
                           const double d { std::clamp(in.dt, 0.0f, 0.05f) };
                           const double blend { std::min(1.0, d * 3.0) };

                           return s + (in.energy - s) * blend;
                       }))
    };

    // The field environment runs on the worker cadence: its own dt, and the
    // stir from ui re-anchored on worker through a `latest` crossing.
    const auto worker_dt { share(input<double, clock_tag::worker>("dt_worker")) };
    const auto stir_worker { share(at<clock_tag::worker, policy::latest>(stir)) };

    // The spirit plane (carried by the kernel's audio clock tag). Zero
    // crossings in: the tide knows nothing about the ball, the pointer or the
    // field — that is the point. Only its own clock drive; phases complete in
    // tens of minutes, so the scene's disposition drifts whether or not anyone
    // is playing. Crossed out through `latest` (below).
    const auto spirit_dt { share(input<double, clock_tag::audio>("dt_spirit")) };

    const auto tide_wander
    {
        share(
            spirit_dt
                >> lift<double, double>(
                       [](const double& d) { return 0.011 * d; })
                >> scan<double, double>(
                       0.0,
                       [](const double& s, const double& d) { return std::fmod(s + d, 6.283185307179586); }))
    };

    const auto tide_mood
    {
        share(
            spirit_dt
                >> lift<double, double>(
                       [](const double& d) { return 0.007 * d; })
                >> scan<double, double>(
                       1.7,
                       [](const double& s, const double& d) { return std::fmod(s + d, 6.283185307179586); }))
    };

    const auto tide_base
    {
        share(
            spirit_dt
                >> lift<double, double>(
                       [](const double& d) { return 0.004 * d; })
                >> scan<double, double>(
                       3.1,
                       [](const double& s, const double& d) { return std::fmod(s + d, 6.283185307179586); }))
    };

    const auto tide
    {
        share(
            zip(tide_wander, tide_mood, tide_base)
                >> lift<std::pair<pair_t, double>, spirit_tide>(
                       [](const double& a, const double& b, const double& c)
                       {
                           return spirit_tide
                           {
                               static_cast<float>(std::clamp(0.5 + 0.40 * std::sin(a) + 0.16 * std::sin(b * 1.7), 0.0, 1.0)),
                               static_cast<float>(std::clamp(0.5 + 0.44 * std::sin(b + 1.3), 0.0, 1.0)),
                               static_cast<float>(std::clamp(0.66 + 0.24 * std::sin(c * 1.4 + 0.7) + 0.10 * std::cos(a * 0.6), 0.0, 1.0)),
                               static_cast<float>(std::fmod(0.37 + 0.11 * std::sin(c), 1.0))
                           };
                       }))
    };

    // Since the tide lands here under its own name, downstream folds lean on
    // its fields (calm, warmth, restlessness), never on "a slow sine". It is
    // read through `latest`, so worker folds see a fresh-but-unchanging
    // snapshot at their cadence.
    const auto tide_worker { share(at<clock_tag::worker, policy::latest>(tide)) };

    const auto spin { share(phase_chain<0.045>(worker_dt, stir_worker, 0.0)) };
    const auto breathe { share(phase_chain<0.026>(worker_dt, stir_worker, 1.7)) };
    const auto sway { share(phase_chain<0.017>(worker_dt, stir_worker, 3.1)) };

    // Angle and curl in one fold — the two shape coefficients the renderer
    // bends stroke geometry with, saved from separate chains so the tide's
    // output fits `parallel`'s 8-branch cap.
    const auto curve
    {
        zip(spin, breathe)
            >> lift<pair_t, field_curve>(
                   [](const double& p, const double& b)
                   {
                       return field_curve
                       {
                           0.62 * std::sin(p) + 0.34 * std::sin(p * 0.5 + 1.3),
                           1.05 + 0.42 * std::sin(b) + 0.22 * std::cos(p * 0.7)
                       };
                   })
    };

    const auto scale
    {
        zip(sway, breathe)
            >> lift<pair_t, double>(
                   [](const double& s, const double& b)
                   {
                       return 1.85 + 0.55 * std::sin(s) + 0.35 * std::cos(b * 0.8);
                   })
    };

    // A five-branch zip: phase, phase, phase, stir, tide.
    using five_wire_t = std::pair<quad_wire_t, spirit_tide>;

    // The scene's sound: the same phases and stir, folded into a hum, plus
    // the tide's role at the moment of the fold: calm scales the flow
    // baseline, warmth sets the pitch's floor bias. Three floats — inline.
    const auto audio
    {
        zip(spin, breathe, sway, stir_worker, tide_worker)
            >> lift<five_wire_t, scene_audio>(
                   [](const double& p, const double& b, const double& s, const double& k, const spirit_tide& t)
                   {
                       const double pull { std::clamp(k, 0.0, 1.6) / 1.6 };

                       return scene_audio
                       {
                           // Rest: a low, slowly breathing tone (~147 Hz);
                           // stir raises it a fourth-ish and spin detunes a
                           // hair; warmth biases the whole band.
                           static_cast<float>(147.0 * (1.0 + 0.24 * pull) * (0.985 + 0.03 * t.warmth) * (1.0 + 0.022 * std::sin(p * 0.9 + 1.0))),
                           static_cast<float>(std::clamp(
                               0.036 + 0.02 * t.restlessness + 0.05 * std::abs(std::sin(p * 0.35)) + 0.5 * pull,
                               0.0,
                               0.9)),
                           // Near-stagnant at rest: a whisper of drift. calm
                           // is the tide's lever on the whole ambience (set
                           // low by design: the field idles slower now).
                           static_cast<float>((6.6 + 2.6 * std::sin(s + 0.4) + 1.7 * std::cos(b * 0.7) + 105.0 * k) * (0.78 + 0.24 * t.calm))
                       };
                   })
    };

    // The field's push on the ball: two slow lane-shaped drifts, stronger the
    // more the ball stirs. Canvas/s; the host copies it into pushX/pushY.
    // Scaled by the same calm factor as the flow, so field-following and
    // push stay proportional.
    const auto current
    {
        zip(spin, sway, stir_worker, tide_worker)
            >> lift<std::pair<std::pair<pair_t, double>, spirit_tide>, field_current>(
                   [](const double& p, const double& s, const double& k, const spirit_tide& t)
                   {
                       const double floor { 0.78 };

                       return field_current
                       {
                           0.16 * std::sin(p * 0.8) * (0.25 + k) * (floor + 0.24 * t.calm),
                           0.12 * std::cos(s * 0.9 + 1.3) * (0.25 + k) * (floor + 0.24 * t.calm)
                       };
                   })
    };

    return parallel(
        ball_x >> output<ball_state, clock_tag::ui>("ballX"),
        ball_y >> output<ball_state, clock_tag::ui>("ballY"),
        stir >> output<double, clock_tag::ui>("stir"),
        curve >> output<field_curve, clock_tag::worker>("curve"),
        scale >> output<double, clock_tag::worker>("scale"),
        audio >> output<scene_audio, clock_tag::worker>("audio"),
        current >> output<field_current, clock_tag::worker>("current"),
        tide >> output<spirit_tide, clock_tag::audio>("tide"));
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

double
fetch_current(const kernel::value& value, bool want_y)
{
    const auto* in { std::get_if<kernel::user_inline>(&value) };

    if (!in || in->type != &kernel::tag_of<field_current>())
        return std::numeric_limits<double>::quiet_NaN();

    const auto& c { *reinterpret_cast<const field_current*>(in->bytes.data()) };

    return want_y ? c.y : c.x;
}

// Timeline: 0 = pitch (Hz), 1 = level (0..1), 2 = field speed.
double
fetch_audio(const kernel::value& value, int which)
{
    const auto* in { std::get_if<kernel::user_inline>(&value) };

    if (!in || in->type != &kernel::tag_of<scene_audio>())
        return std::numeric_limits<double>::quiet_NaN();

    const auto& a { *reinterpret_cast<const scene_audio*>(in->bytes.data()) };

    switch (which)
    {
        case 0: return a.pitch;
        case 1: return a.level;
        default: return a.speed;
    }
}

double
fetch_curve(const kernel::value& value, bool want_curl)
{
    const auto* in { std::get_if<kernel::user_inline>(&value) };

    if (!in || in->type != &kernel::tag_of<field_curve>())
        return std::numeric_limits<double>::quiet_NaN();

    const auto& c { *reinterpret_cast<const field_curve*>(in->bytes.data()) };

    return want_curl ? c.curl : c.angle;
}

double
fetch_tide(const kernel::value& value, int which)
{
    const auto* in { std::get_if<kernel::user_inline>(&value) };

    if (!in || in->type != &kernel::tag_of<spirit_tide>())
        return std::numeric_limits<double>::quiet_NaN();

    const auto& t { *reinterpret_cast<const spirit_tide*>(in->bytes.data()) };

    switch (which)
    {
        case 0: return t.restlessness;
        case 1: return t.warmth;
        case 2: return t.calm;
        default: return t.hue;
    }
}

class StirredField
{
public:
    StirredField()
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
    setSwim(double x, double y)
    {
        if (!m_program)
            return;

        m_program->set("swimX", kernel::value { std::clamp(std::isfinite(x) ? x : 0.0, -1.0, 1.0) });
        m_program->set("swimY", kernel::value { std::clamp(std::isfinite(y) ? y : 0.0, -1.0, 1.0) });
    }

    void
    tick(double dt)
    {
        if (!m_program)
            return;

        const double step { std::isfinite(dt) ? std::clamp(dt, 0.0, 0.05) : 1.0 / 60.0 };

        m_program->set("dt", kernel::value { step });
        m_engine.step(*m_program, clock_tag::ui);

        // The environment advances every third frame with a compensated dt:
        // same total phase speed, but its values refresh in visible chunks.
        if ((m_frame % worker_period) == 0)
        {
            m_program->set("dt_worker", kernel::value { step * worker_period });
            m_engine.step(*m_program, clock_tag::worker);

            // The loop's return edge: this frame's fresh push rides ui input
            // ports into the next ui step (one frame of latency, by design).
            m_program->set("pushX", kernel::value { fetch_current(m_program->get("current"), false) });
            m_program->set("pushY", kernel::value { fetch_current(m_program->get("current"), true) });
        }

        ++m_frame;
    }

    // The spirit clock is stepped by its own rhythm, not the frame loop: the
    // host calls this on a slow timer so the tide keeps rolling even when
    // the tab is hidden and the canvas idle. Spirit's inputs: none.
    void
    stepTide(double dtSec)
    {
        if (!m_program)
            return;

        m_program->set(
            "dt_spirit",
            kernel::value { std::clamp(std::isfinite(dtSec) ? dtSec : 2.0, 0.0, 30.0) });
        m_engine.step(*m_program, clock_tag::audio);
    }

    void
    reset()
    {
        m_program.emplace(stirred_field_recipe());

        m_down = false;
        m_frame = 0;

        m_program->set("targetX", kernel::value { 0.5 });
        m_program->set("targetY", kernel::value { 0.5 });
        m_program->set("dt", kernel::value { 1.0 / 60.0 });
        m_program->set("dt_worker", kernel::value { worker_period / 60.0 });
        m_program->set("grabbed", kernel::value { false });
        m_program->set("pushX", kernel::value { 0.0 });
        m_program->set("pushY", kernel::value { 0.0 });
        m_program->set("swimX", kernel::value { 0.0 });
        m_program->set("swimY", kernel::value { 0.0 });
        m_program->set("dt_spirit", kernel::value { 2.0 });

        // Warm-up: the ui clock publishes ball + stir first, so the worker's
        // `latest` crossing has a value to sample on its first step, and the
        // spirit clock publishes the tide before the worker's fold reads it.
        // Every getter is finite on frame zero.
        m_engine.step(*m_program, clock_tag::ui);
        m_engine.step(*m_program, clock_tag::audio);
        m_engine.step(*m_program, clock_tag::worker);

        m_program->set("pushX", kernel::value { fetch_current(m_program->get("current"), false) });
        m_program->set("pushY", kernel::value { fetch_current(m_program->get("current"), true) });
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
    stir() const
    {
        return m_program ? lab::read_double(*m_program, "stir") : std::numeric_limits<double>::quiet_NaN();
    }

    double
    angle() const
    {
        return m_program ? fetch_curve(m_program->get("curve"), false) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    curl() const
    {
        return m_program ? fetch_curve(m_program->get("curve"), true) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    restlessness() const
    {
        return m_program ? fetch_tide(m_program->get("tide"), 0) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    warmth() const
    {
        return m_program ? fetch_tide(m_program->get("tide"), 1) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    calm() const
    {
        return m_program ? fetch_tide(m_program->get("tide"), 2) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    hue() const
    {
        return m_program ? fetch_tide(m_program->get("tide"), 3) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    scale() const
    {
        return m_program ? lab::read_double(*m_program, "scale") : std::numeric_limits<double>::quiet_NaN();
    }

    double
    speed() const
    {
        return m_program ? fetch_audio(m_program->get("audio"), 2) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    audioPitch() const
    {
        return m_program ? fetch_audio(m_program->get("audio"), 0) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    audioLevel() const
    {
        return m_program ? fetch_audio(m_program->get("audio"), 1) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    curX() const
    {
        return m_program ? fetch_current(m_program->get("current"), false) : std::numeric_limits<double>::quiet_NaN();
    }

    double
    curY() const
    {
        return m_program ? fetch_current(m_program->get("current"), true) : std::numeric_limits<double>::quiet_NaN();
    }

private:
    static constexpr int worker_period { 3 };

    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;

    int m_frame { 0 };
    bool m_down { false };
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(lab_stirred_field)
{
    emscripten::class_<StirredField>("StirredField")
        .constructor()
        .function("tick", &StirredField::tick)
        .function("stepTide", &StirredField::stepTide)
        .function("setPointer", &StirredField::setPointer)
        .function("setSwim", &StirredField::setSwim)
        .function("reset", &StirredField::reset)
        .function("ballX", &StirredField::ballX)
        .function("ballY", &StirredField::ballY)
        .function("velX", &StirredField::velX)
        .function("velY", &StirredField::velY)
        .function("stir", &StirredField::stir)
        .function("angle", &StirredField::angle)
        .function("curl", &StirredField::curl)
        .function("restlessness", &StirredField::restlessness)
        .function("warmth", &StirredField::warmth)
        .function("calm", &StirredField::calm)
        .function("hue", &StirredField::hue)
        .function("scale", &StirredField::scale)
        .function("speed", &StirredField::speed)
        .function("audioPitch", &StirredField::audioPitch)
        .function("audioLevel", &StirredField::audioLevel)
        .function("curX", &StirredField::curX)
        .function("curY", &StirredField::curY);
}
#pragma clang diagnostic pop
