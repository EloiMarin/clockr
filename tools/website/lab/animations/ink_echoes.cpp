// Ink Echoes — draw with the pointer; parallel delay lanes keep painting.
//
// ui clock: the raw pointer levels fan out into four delay chains. Each chain
// is a different depth of unit delays, so the same gesture is re-drawn by
// lanes that remember it for 3, 7, 14 and 22 ticks — the long lanes catch up
// and finish a stroke after the pointer has already moved on. JS only draws:
// it keeps painting whatever each lane's current position is, so the ink
// accumulates and each depth leaves its own calligraphic echo.

#include "lab_common.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/recipe.hpp"

#include <emscripten/bind.h>

#include <optional>
#include <utility>

namespace
{

using namespace clockr;

using kernel::clock_tag;

// N chained unit delays over the same seed: a fragment outputs the pointer
// from N ticks ago. The seed at the canvas centre keeps every lane warm until
// the pointer arrives. Recursion, not a loop: the arena grows a type per
// delay, so the accumulator cannot be reassigned.
template<std::size_t N, typename src_t>
auto
echo_chain(const src_t& src, double seed)
{
    using namespace clockr::shell;

    const auto next { src >> delay<double>(seed) };

    if constexpr (N > 1)
        return echo_chain<N - 1>(next, seed);
    else
        return next;
}

template<std::size_t N>
auto
echo_lane(const auto& src, double seed)
{
    return echo_chain<N>(src, seed);
}

auto
ink_echoes_recipe()
{
    using namespace clockr::shell;

    const auto px { share(input<double, clock_tag::ui>("px")) };
    const auto py { share(input<double, clock_tag::ui>("py")) };

    return parallel(
        echo_lane<3>(px, 0.5) >> output<double, clock_tag::ui>("l1x"),
        echo_lane<3>(py, 0.5) >> output<double, clock_tag::ui>("l1y"),
        echo_lane<7>(px, 0.5) >> output<double, clock_tag::ui>("l2x"),
        echo_lane<7>(py, 0.5) >> output<double, clock_tag::ui>("l2y"),
        echo_lane<14>(px, 0.5) >> output<double, clock_tag::ui>("l3x"),
        echo_lane<14>(py, 0.5) >> output<double, clock_tag::ui>("l3y"),
        echo_lane<22>(px, 0.5) >> output<double, clock_tag::ui>("l4x"),
        echo_lane<22>(py, 0.5) >> output<double, clock_tag::ui>("l4y"));}

class InkEchoes
{
public:
    InkEchoes()
    {
        reset();
    }

    void
    setPointer(double x, double y)
    {
        if (!m_program)
            return;

        m_program->set("px", kernel::value { x });
        m_program->set("py", kernel::value { y });
    }

    void
    tick(double)
    {
        if (!m_program)
            return;

        m_engine.step(*m_program, clock_tag::ui);
    }

    void
    reset()
    {
        m_program.emplace(ink_echoes_recipe());

        m_program->set("px", kernel::value { 0.5 });
        m_program->set("py", kernel::value { 0.5 });

        m_engine.step(*m_program, clock_tag::ui);
    }

    // k is the lane index, 0..3 in increasing depth.
    double
    laneX(int k) const
    {
        if (!m_program)
            return std::numeric_limits<double>::quiet_NaN();

        return lab::read_double(*m_program, k == 0 ? "l1x" : k == 1 ? "l2x" : k == 2 ? "l3x" : "l4x");
    }

    double
    laneY(int k) const
    {
        if (!m_program)
            return std::numeric_limits<double>::quiet_NaN();

        return lab::read_double(*m_program, k == 0 ? "l1y" : k == 1 ? "l2y" : k == 2 ? "l3y" : "l4y");
    }

    double
    pointerX() const
    {
        if (!m_program)
            return std::numeric_limits<double>::quiet_NaN();

        const kernel::value value { m_program->get_input("px") };
        const auto* d { std::get_if<double>(&value) };

        return d ? *d : std::numeric_limits<double>::quiet_NaN();
    }

    double
    pointerY() const
    {
        if (!m_program)
            return std::numeric_limits<double>::quiet_NaN();

        const kernel::value value { m_program->get_input("py") };
        const auto* d { std::get_if<double>(&value) };

        return d ? *d : std::numeric_limits<double>::quiet_NaN();
    }

private:
    std::optional<kernel::program> m_program;
    kernel::release_engine m_engine;
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(lab_ink_echoes)
{
    emscripten::class_<InkEchoes>("InkEchoes")
        .constructor()
        .function("setPointer", &InkEchoes::setPointer)
        .function("tick", &InkEchoes::tick)
        .function("reset", &InkEchoes::reset)
        .function("laneX", &InkEchoes::laneX)
        .function("laneY", &InkEchoes::laneY)
        .function("pointerX", &InkEchoes::pointerX)
        .function("pointerY", &InkEchoes::pointerY);
}
#pragma clang diagnostic pop
