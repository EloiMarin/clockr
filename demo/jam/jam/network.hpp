#pragma once

/// @file
/// The jam's fixed network: editable score state + reusable DAW transport +
/// step scheduler + fixed voice pool.
///
/// The topology never changes. Patches cross ui -> audio as queued events and
/// update a `scan`'d score; every musical decision is a pure function over
/// that state (scheduler.hpp, voice.hpp). The DAW's transport functions and
/// wire structs are reused unchanged (daw/transport.hpp).
///
/// Antecedents (daw/network.hpp): each source leaf is created once and
/// shared, multi-consumer fragments are shared, every clock change is an
/// explicit `at<clock, policy>`, and the playhead crosses back audio -> ui as
/// a latest sample.

#include "jam/patch.hpp"
#include "jam/scheduler.hpp"
#include "jam/score.hpp"
#include "jam/seeded_scan.hpp"
#include "jam/voice.hpp"

#include "daw/transport.hpp"
#include "clockr/kernel/engine.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/shell/recipe.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace jam::net
{

using clockr::kernel::clock_tag;

// zip(a, b, c, d) : pair<pair<pair<A, B>, C>, D>; a lift takes one argument
// per leaf, left to right (law L8). Named so a leaf reorder fails here.
using transport_wire = std::pair<
    std::pair<std::pair<daw::BlockDesc, daw::LoopRegion>, bool>,
    double>;
// (block, loop, playing, bpm) -> TransportIn

using schedule_wire = std::pair<
    std::pair<
        std::pair<std::pair<score, daw::PosPair>, daw::LoopRegion>,
        daw::BlockDesc>,
    double>;
// (score, pos_pair, loop, block, bpm) -> event_batch

using mix_wire = std::pair<std::pair<voice_state, score>, double>;
// (voices, score, master) -> mix

/// Build the jam network around `initial`. By value, like every recipe: each
/// program constructed from it has its own score state.
inline auto
app(score initial)
{
    using namespace clockr;
    using namespace clockr::shell;

    const auto score_seed { std::make_shared<const score>(std::move(initial)) };
    const auto voice_seed { std::make_shared<const voice_state>(voice_state {}) };

    const auto playing
    {
        at<clock_tag::audio, conc::policy::latest>(
            input<bool, clock_tag::ui>("playing")
        )
    };
    const auto bpm
    {
        share(
            at<clock_tag::audio, conc::policy::latest>(
                input<double, clock_tag::ui>("bpm")
            )
        )
    };
    const auto loop
    {
        share(
            at<clock_tag::audio, conc::policy::latest>(
                input<daw::LoopRegion, clock_tag::ui>("loop")
            )
        )
    };
    const auto master
    {
        at<clock_tag::audio, conc::policy::latest>(
            input<double, clock_tag::ui>("master")
        )
    };
    // A patch is an event, not a level: queued keeps every distinct edit
    // (no coalescing), and a disengaged tick is a no-op for the score scan.
    const auto patch
    {
        at<clock_tag::audio, conc::policy::queued>(
            input<std::optional<score_patch>, clock_tag::ui>("patch")
        )
    };
    const auto block
    {
        share(
            input<daw::BlockDesc, clock_tag::audio>("block")
        )
    };

    // One audio tick's transport intent bundle (the jam has no seek port).
    const auto bundle
    {
        zip(block, loop, playing, bpm)
            >> lift<transport_wire, daw::TransportIn>(
                   [](const daw::BlockDesc& b,
                      const daw::LoopRegion& l,
                      const bool& p,
                      const double& tempo)
                   {
                       return daw::TransportIn
                       {
                           .block = b,
                           .loop = l,
                           .playing = p,
                           .seek = std::nullopt,
                           .bpm = tempo,
                       };
                   })
    };

    // The transport scan: s(t) = {s(t-1).cur, step(s(t-1).cur, in(t))}.
    const auto pos_pair
    {
        share(
            bundle
                >> scan<daw::TransportIn, daw::PosPair>(
                    daw::PosPair { 0.0, 0.0 },
                    [](const daw::PosPair& state, const daw::TransportIn& in)
                    {
                        return daw::PosPair
                        {
                            state.cur,
                            daw::transport_step(state.cur, in),
                        };
                    })
        )
    };

    // The playhead has two consumers: the audio output and the ui feedback.
    const auto playhead
    {
        share(
            pos_pair
                >> lift<daw::PosPair, double>(
                    [](const daw::PosPair& p) { return p.cur; })
        )
    };

    // Editable score state. A patch is applied once when its ui tick publishes
    // it; the next tick publishes a disengaged event, so the scan is stable.
    const auto score_state
    {
        share(
            patch
                >> scan_seeded<std::optional<score_patch>, score>(
                    score_seed,
                    [](const score& current,
                       const std::optional<score_patch>& edit)
                    {
                        return edit ? jam::apply(current, *edit) : current;
                    })
        )
    };

    // Pure scheduling over the current score and the tick's interval. Shared
    // by the event output and the voice scan, so it runs once per audio tick.
    const auto events
    {
        share(
            zip(score_state, pos_pair, loop, block, bpm)
                >> lift<schedule_wire, event_batch>(
                    [](const score& s,
                       const daw::PosPair& p,
                       const daw::LoopRegion& l,
                       const daw::BlockDesc& b,
                       const double& tempo)
                    {
                        return jam::schedule(s, p, l, b, tempo);
                    })
        )
    };

    const auto voices
    {
        share(
            events
                >> scan_seeded<event_batch, voice_state>(
                    voice_seed,
                    [](const voice_state& state, const event_batch& batch)
                    {
                        return jam::voice_tick(state, batch);
                    })
        )
    };

    const auto mix
    {
        zip(voices, score_state, master)
            >> lift<mix_wire, double>(
                   [](const voice_state& v,
                      const score& s,
                      const double& level)
                   {
                       return jam::voice_mix(v, s, level);
                   })
    };

    // The display feedback: the audio playhead sampled on the ui clock.
    const auto cursor
    {
        at<clock_tag::ui, conc::policy::latest>(playhead)
            >> output<double, clock_tag::ui>("playhead_ui")
    };

    return parallel(
        playhead >> output<double, clock_tag::audio>("playhead"),
        events >> output<event_batch, clock_tag::audio>("events"),
        voices >> output<voice_state, clock_tag::audio>("voices"),
        mix >> output<double, clock_tag::audio>("mix"),
        cursor
    );
}

/// Level the jam's intent ports and tick ui once. Call before the first audio
/// tick, exactly like `daw::init_dawmulator`: the crossed levels must have a
/// producer tick before the audio region consumes them.
inline void
init(clockr::kernel::program& prog,
     clockr::kernel::engine& eng,
     const score& initial)
{
    prog.set("playing", false);
    prog.set("bpm", static_cast<double>(initial.bpm));
    prog.set<daw::LoopRegion>("loop", 0.0, 4.0, true);
    prog.set("master", 0.8);
    eng.step(prog, clockr::kernel::clock_tag::ui);
}

} // namespace jam::net
