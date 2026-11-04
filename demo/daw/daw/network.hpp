#pragma once

// DAWmulator network — P1: audio-only voices over a scripted score (README
// §5, §7). Transport + loop + single-bpm tempo + monophonic MIDI scheduler
// with sample-accurate offsets + one gated voice (on/off crossing twins
// folded into a gate event), ui and audio clocks crossed with both crossings
// (latest snapshots, queued seek events), and the playhead crossed back
// audio→ui (latest) so the ui region can observe the transport it drives
// (the interactive shell's timeline cursor).
//
// Cohesive struct wires (kernel user values): ports carry LoopRegion and
// BlockDesc whole, one scan over TransportIn holds the whole tick bundle, and
// PosPair/Voice persist as struct state. Small structs (PosPair, Voice) ride
// inline; the rest ride ref-counted pool cells — the paying option, used
// where structs cross clocks or persist in state. Purely authoring-level
// bundling stays phantom pairs (see README §4); the wiring adds no logic:
// every musical decision is a pure function in daw/transport.hpp.
//
// Sharing discipline: each source leaf is created ONCE and shared (share is
// nominal per call site — calling a leaf helper twice would register its port
// twice), and so is every multi-consumer fragment. Cross-clock leaves cross
// once (at<>) and fan out audio-side; the transport share fans out to
// playhead/scheduler/voice (L2/L4), and the playhead share fans out across
// the audio→ui crossing to the display feedback.

#include "daw/transport.hpp"

#include "clockr/shell/recipe.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace daw::net
{

using clockr::kernel::clock_tag;

// One scripted note: the schedulers close over it (build-time constant,
// never a wire value). The note has a start beat and a length — the off
// crossing is reached `len` beats after the on, through any loop wrap
// (daw::note_end_beat), so a note longer than the loop's remainder ends on
// the next lap. P1 voices monophonically; the score stays a table so the
// polyphonic lift keeps its shape for later.
struct score
{
    double beat { 3.5 };
    double len { 1.0 };
    std::int64_t id { 60 };
};

// --- wire nests, left-associative in zip order (L8) --------------------------
//
// zip(a, b, c) : pair<pair<A, B>, C>; lift over it takes one argument per
// leaf, left to right. Shallow by design: structs carry the cohesive bundles,
// so pairs only assemble leaves that never merited a struct. Named here so a
// leaf reorder fails at the alias, not deep in a lambda body.

using bundle_wire = std::pair<
    std::pair<std::pair<std::pair<BlockDesc, LoopRegion>, bool>, std::optional<double>>,
    double>;
// (block, loop, playing, seek, bpm) -> one TransportIn

using count_wire = std::pair<std::pair<double, PosPair>, LoopRegion>;
// (block_start, pos_pair, loop) -> trigger count

using off_wire = std::pair<
    std::pair<std::pair<std::pair<double, PosPair>, LoopRegion>, BlockDesc>,
    double>;
// (block_start, pos_pair, loop, block, bpm) -> first-trigger offset

using gate_wire = std::pair<std::int64_t, std::int64_t>;
// (on_count, off_count) -> gate event

// --- the app ----------------------------------------------------------------

inline auto
app(score s)
{
    using namespace clockr;
    using namespace clockr::shell;

    const auto playing
    {
        at<clock_tag::audio, conc::policy::latest>(
            input<bool, clock_tag::ui>("playing")
        )
    };
    const auto seek
    {
        share(
            at<clock_tag::audio, conc::policy::queued>(
                input<std::optional<double>, clock_tag::ui>("seek")
            )
        )
    };
    const auto loop
    {
        share(
            at<clock_tag::audio, conc::policy::latest>(
                input<LoopRegion, clock_tag::ui>("loop")
            )
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
    const auto master
    {
        at<clock_tag::audio, conc::policy::latest>(
            input<double, clock_tag::ui>("master")
        )
    };
    const auto block
    {
        share(
            input<BlockDesc, clock_tag::audio>("block")
        )
    };

    // One audio tick's intent bundle, assembled where it is consumed.
    const auto bundle
    {
        zip(block, loop, playing, seek, bpm)
            >> lift<bundle_wire, TransportIn>(
                   [](const BlockDesc& b, const LoopRegion& l, const bool& p, const std::optional<double>& sk, const double& bpm_v)
                   {
                       return TransportIn
                       {
                           .block = b,
                           .loop = l,
                           .playing = p,
                           .seek = sk,
                           .bpm = bpm_v,
                       };
                   })
    };

    // THE transport scan: s(t) = {s(t-1).cur, step(s(t-1).cur, in(t))}.
    // The pair state gives schedulers the crossing interval (prev, cur] with
    // no second delay tap. One playhead (L4).
    const auto pos_pair
    {
        share(
            bundle
                >> scan<TransportIn, PosPair>(
                    PosPair { 0.0, 0.0 },
                    [](const PosPair& state, const TransportIn& in)
                    {
                        return PosPair { state.cur, daw::transport_step(state.cur, in) };
                    }
                )
        )
    };

    // The playhead as one shared node with two consumers: the audio output
    // and the ui back-crossing below (L4 — one lift, one evaluation per
    // audio tick, however the ui samples it).
    const auto playhead_position
    {
        share(
            pos_pair
                >> lift<PosPair, double>([](const PosPair& p) { return p.cur; })
        )
    };

    // This block's start beat: the seek when seeking, else the previous pos.
    const auto block_start
    {
        share(
            zip(pos_pair, seek)
                >> lift<std::pair<PosPair, std::optional<double>>, double>(
                       [](const PosPair& p, const std::optional<double>& sk)
                       {
                           return daw::block_start_beat(p.prev, sk);
                       }
                   )
        )
    };

    // Scheduler twins: the count reads (start, pair, loop), the offset twin
    // additionally the block and tempo. Every parameter is read — no silent
    // positional padding.
    const auto midi_count
    {
        share(
            zip(block_start, pos_pair, loop)
                >> lift<count_wire, std::int64_t>(
                    [s](const double& bs, const PosPair& p, const LoopRegion& l)
                    {
                        return daw::schedule_count(bs, p.cur, l.start, l.end, l.enabled, s.beat);
                    }
                )
        )
    };

    // The note-off twin: same shape over the note's end beat, so the voice
    // can close. Off crossings with nothing sounding are no-ops downstream
    // (the gate latches idempotently), which is also what makes the wrapped
    // end beat harmless when the transport crosses it before any note-on.
    auto midi_off_count = share(
        zip(block_start, pos_pair, loop)
        >> lift<count_wire, std::int64_t>(
               [s](const double& bs, const PosPair& p, const LoopRegion& l)
               {
                   return daw::schedule_count(
                       bs,
                       p.cur,
                       l.start,
                       l.end,
                       l.enabled,
                       daw::note_end_beat(s.beat, s.len, l.start, l.end, l.enabled));
               }));

    auto midi_offset =
        zip(block_start, pos_pair, loop, block, bpm)
        >> lift<off_wire, std::int64_t>(
               [s](const double& bs,
                   const PosPair& p,
                   const LoopRegion& l,
                   const BlockDesc& b,
                   const double& bpm_v)
               {
                   return daw::schedule_offset(
                       bs,
                       p.cur,
                       l.start,
                       l.end,
                       l.enabled,
                       b.start_sample,
                       b.n_frames,
                       b.sample_rate,
                       bpm_v,
                       s.beat);
               });

    // The voice gate: the twin counts fold into one event per block.
    auto gate = zip(midi_count, midi_off_count)
        >> lift<gate_wire, std::int64_t>(
               [](const std::int64_t& on, const std::int64_t& off)
               { return daw::voice_gate(on, off); });

    // Monophonic voice: the gate opens the note id, the off closes it,
    // silence sustains.
    auto voice = gate
        >> scan<std::int64_t, Voice>(
               Voice { false, -1 },
               [id = s.id](const Voice& v, const std::int64_t& g)
               { return daw::voice_latch(v, g, id); });

    auto gains = zip(voice, master)
        >> lift<std::pair<Voice, double>, double>(
               [](const Voice& v, const double& m) { return daw::voice_gain(v, m); });

    // The display feedback: the audio playhead sampled on the ui clock
    // (latest). The shell's timeline cursor reads `playhead_ui` at ui
    // cadence; it holds the last sample while audio is quiet — the same
    // node as the audio playhead output crosses, it is not recomputed.
    const auto cursor
    {
        at<clock_tag::ui, conc::policy::latest>(playhead_position)
            >> output<double, clock_tag::ui>("playhead_ui")
    };

    return parallel(
        playhead_position >> output<double, clock_tag::audio>("playhead"),
        midi_count >> output<std::int64_t, clock_tag::audio>("midi_count"),
        midi_offset >> output<std::int64_t, clock_tag::audio>("midi_offset"),
        midi_off_count >> output<std::int64_t, clock_tag::audio>("midi_off_count"),
        gains >> output<double, clock_tag::audio>("voice_gain"),
        cursor
    );
}

} // namespace daw::net
