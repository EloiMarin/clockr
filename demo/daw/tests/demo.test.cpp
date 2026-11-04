// DAWmulator — first-cut acceptance (§6.1) plus the sharing/parity pins.
//
// The dumb shell contract, in full: set() ports, tick() a region, get()
// outputs. No ifs here — every branch below is test sequencing, never demo
// behaviour. Behaviour lives in daw/transport.hpp (pure) and daw/network.hpp
// (wiring); this file only drives clocks and asserts musical facts.

#include "daw/dawmulator.hpp"
#include "daw/network.hpp"

#include "clockr/kernel/release_engine.hpp"
#include "clockr/kernel/debug_engine.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/shell/descriptor_storage.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace clockr;

namespace
{

constexpr double kRate { 48000.0 };
constexpr std::int64_t kBlock { 24000 }; // 1 beat blocks at 120 bpm
constexpr daw::net::score kScore { 3.5, 1.0, 60 }; // note at 3.5, one beat long

struct readings
{
    double playhead {};
    std::int64_t midi_count {};
    std::int64_t midi_offset {};
    std::int64_t midi_off_count {};
    double voice_gain {};
};

template<typename engine_t>
readings
run_block(kernel::program& prog, engine_t& eng, std::int64_t block_start, std::int64_t n = kBlock)
{
    prog.set<daw::BlockDesc>("block", block_start, n, kRate);
    eng.step(prog, kernel::clock_tag::audio);

    return readings {
        get<double>(prog, "playhead"),
        get<std::int64_t>(prog, "midi_count"),
        get<std::int64_t>(prog, "midi_offset"),
        get<std::int64_t>(prog, "midi_off_count"),
        get<double>(prog, "voice_gain"),
    };
}

void
set_ui(kernel::program& prog,
       bool playing = true,
       double bpm = 120.0,
       double loop_start = 0.0,
       double loop_end = 4.0,
       bool loop_on = true,
       double master = 0.8)
{
    prog.set("playing", playing);
    prog.set("bpm", bpm);
    prog.set<daw::LoopRegion>("loop", loop_start, loop_end, loop_on);
    prog.set("master", master);
}

template<kernel::graph_c graph_t>
std::size_t
count_kind(const graph_t& g, kernel::node_kind kind_wanted)
{
    std::size_t n { 0 };

    for (const auto& node : nodes(g))
        if (kind(node) == kind_wanted)
            ++n;

    return n;
}

} // namespace

TEST_CASE("daw | First-cut acceptance: transport, loop, tempo, both crossings, offsets", "[daw]")
{
    // §6.1: playing, 120 bpm, loop (0,4] on; five 1-beat blocks. The note
    // [3.5, 4.5) starts at 3.5 — one beat long, so its off crossing lands at
    // the wrapped beat 0.5: the on fires exactly on the spanning block with
    // its sample offset, the voice opens there and releases one block later,
    // when the next lap crosses 0.5; the loop wraps with remainder;
    // everything else is silence.
    kernel::program prog { daw::net::app(kScore) };
    kernel::release_engine eng;

    set_ui(prog);
    eng.step(prog, kernel::clock_tag::ui);

    const auto b0 { run_block(prog, eng, 0) };
    const auto b1 { run_block(prog, eng, kBlock) };
    const auto b2 { run_block(prog, eng, 2 * kBlock) };
    const auto b3 { run_block(prog, eng, 3 * kBlock) };
    const auto b4 { run_block(prog, eng, 4 * kBlock) };

    REQUIRE(b0.playhead == Catch::Approx(1.0));
    REQUIRE(b1.playhead == Catch::Approx(2.0));
    REQUIRE(b2.playhead == Catch::Approx(3.0));
    REQUIRE(b3.playhead == Catch::Approx(0.0)); // 4.0 wraps with remainder
    REQUIRE(b4.playhead == Catch::Approx(1.0));

    REQUIRE(b0.midi_count == 0);
    REQUIRE(b1.midi_count == 0);
    REQUIRE(b2.midi_count == 0);
    REQUIRE(b3.midi_count == 1);
    REQUIRE(b4.midi_count == 0);

    REQUIRE(b0.midi_offset == -1);
    REQUIRE(b3.midi_offset == 12000); // half a beat into the block
    REQUIRE(b4.midi_offset == -1);

    // The wrapped off beat 0.5 is crossed before any note sounds — a no-op
    // on the already-silent voice — and again right after the note, where
    // it releases.
    REQUIRE(b0.midi_off_count == 1);
    REQUIRE(b1.midi_off_count == 0);
    REQUIRE(b2.midi_off_count == 0);
    REQUIRE(b3.midi_off_count == 0);
    REQUIRE(b4.midi_off_count == 1);

    REQUIRE(b0.voice_gain == Catch::Approx(0.0));
    REQUIRE(b3.voice_gain == Catch::Approx(0.8)); // opened on block 3
    REQUIRE(b4.voice_gain == Catch::Approx(0.0)); // released on block 4
}

TEST_CASE("daw | Debug and release engines agree on the acceptance script", "[daw]")
{
    // L5 over the whole demo: both engines run the §6.1 script and agree on
    // every output, every block.
    kernel::program release_prog { daw::net::app(kScore) };
    kernel::program debug_prog { daw::net::app(kScore) };

    std::string trace;
    kernel::text_trace_sink sink { trace };
    kernel::debug_engine debug { sink };
    kernel::release_engine release;

    set_ui(release_prog);
    set_ui(debug_prog);
    release.step(release_prog, kernel::clock_tag::ui);
    debug.step(debug_prog, kernel::clock_tag::ui);

    for (std::int64_t k { 0 }; k < 5; ++k)
    {
        const auto r { run_block(release_prog, release, k * kBlock) };
        const auto d { run_block(debug_prog, debug, k * kBlock) };

        REQUIRE(d.playhead == Catch::Approx(r.playhead));
        REQUIRE(d.midi_count == r.midi_count);
        REQUIRE(d.midi_offset == r.midi_offset);
        REQUIRE(d.midi_off_count == r.midi_off_count);
        REQUIRE(d.voice_gain == Catch::Approx(r.voice_gain));
    }
}

TEST_CASE("daw | Pause gates, seek jumps paused, resume advances from the landing", "[daw]")
{
    kernel::program prog { daw::net::app(kScore) };
    kernel::release_engine eng;

    set_ui(prog);
    eng.step(prog, kernel::clock_tag::ui);

    REQUIRE(run_block(prog, eng, 0).playhead == Catch::Approx(1.0));

    // Pause: two blocks, no motion, no triggers.
    set_ui(prog, false);
    eng.step(prog, kernel::clock_tag::ui);

    REQUIRE(run_block(prog, eng, kBlock).playhead == Catch::Approx(1.0));
    REQUIRE(run_block(prog, eng, 2 * kBlock).playhead == Catch::Approx(1.0));

    // Seek while paused: the jump lands absolutely, even paused.
    prog.set("seek", shell::to_value(std::optional<double> { 2.5 }));
    eng.step(prog, kernel::clock_tag::ui);

    REQUIRE(run_block(prog, eng, 3 * kBlock).playhead == Catch::Approx(2.5));

    // Resume: the scan carries the landing — the jump entered the state
    // directly through the bundle, so there is no stale integral to resume
    // from.
    set_ui(prog, true);
    eng.step(prog, kernel::clock_tag::ui);

    REQUIRE(run_block(prog, eng, 4 * kBlock).playhead == Catch::Approx(3.5));
    REQUIRE(run_block(prog, eng, 5 * kBlock).playhead == Catch::Approx(0.5));
}

TEST_CASE("daw | Same-epoch double seek keeps the last", "[daw]")
{
    // L9a at the sender: two sets, one ui tick — one queued event, last wins.
    kernel::program prog { daw::net::app(kScore) };
    kernel::release_engine eng;

    set_ui(prog, false);
    prog.set("seek", shell::to_value(std::optional<double> { 16.0 }));
    prog.set("seek", shell::to_value(std::optional<double> { 2.0 }));
    eng.step(prog, kernel::clock_tag::ui);

    REQUIRE(run_block(prog, eng, 0).playhead == Catch::Approx(2.0));
}

TEST_CASE("daw | Each source is instantiated once and fanned out", "[daw]")
{
    // L4 operational: 5 ui + 1 audio port nodes, however many consumers read
    // them. A missing share would register a port name twice and throw at
    // build — or duplicate the node, which this count catches.
    kernel::program prog { daw::net::app(kScore) };

    REQUIRE(count_kind(prog.descriptor(), kernel::node_kind::input) == 6);
    REQUIRE(count_kind(prog.descriptor(), kernel::node_kind::output) == 6);

    // Eleven cross-region edges: the five crossed ui sources fan out to ten
    // audio consumers (the note-off twin reads loop too, so its fan-out grew
    // to four), plus the playhead fed back audio->ui (latest) — the display
    // feedback.
    REQUIRE(prog.topology().crossings.size() == 11);

    // Structural pin of the one-scan transport: one delay inside each of the
    // two state holds (transport pair, voice latch). No delay tap, no rebase
    // anchors — the pair state carries the crossing interval itself.
    REQUIRE(count_kind(prog.descriptor(), kernel::node_kind::delay) == 2);
}

TEST_CASE("daw | Playhead crosses back to ui as latest", "[daw]")
{
    // The display feedback path (README §1): the audio playhead sampled by
    // the ui region. Before the first audio tick the link still carries its
    // initial monostate (the strict read must throw, like any
    // never-produced output); afterwards every ui tick reads the most
    // recent playhead — latest, not queued — and holds it while audio is
    // quiet.
    kernel::program prog { daw::net::app(kScore) };
    kernel::release_engine eng;

    set_ui(prog);
    eng.step(prog, kernel::clock_tag::ui);

    REQUIRE_THROWS_AS(get<double>(prog, "playhead_ui"), std::bad_variant_access);

    // One audio tick: playhead 1.0. The ui region has not ticked since, so
    // the ui output still holds its initial monostate.
    REQUIRE(run_block(prog, eng, 0).playhead == Catch::Approx(1.0));
    REQUIRE_THROWS_AS(get<double>(prog, "playhead_ui"), std::bad_variant_access);

    // The first ui tick after audio samples the latest playhead.
    eng.step(prog, kernel::clock_tag::ui);
    REQUIRE(get<double>(prog, "playhead_ui") == Catch::Approx(1.0));

    // Two more audio ticks land; the ui region still holds the old sample.
    run_block(prog, eng, kBlock);
    run_block(prog, eng, 2 * kBlock);
    REQUIRE(get<double>(prog, "playhead") == Catch::Approx(3.0));
    REQUIRE(get<double>(prog, "playhead_ui") == Catch::Approx(1.0));

    // One ui tick picks up the NEWEST playhead — latest semantics, both
    // audio ticks collapsed into one sample, none of them replayed.
    eng.step(prog, kernel::clock_tag::ui);
    REQUIRE(get<double>(prog, "playhead_ui") == Catch::Approx(3.0));

    // Quiet audio: the sample holds across further ui ticks.
    eng.step(prog, kernel::clock_tag::ui);
    REQUIRE(get<double>(prog, "playhead_ui") == Catch::Approx(3.0));

    // The back-crossing is the only audio->ui edge, and it is latest.
    bool back { false };
    for (const auto& crossing : prog.topology().crossings)
        if (prog.clock_of(crossing.from) == kernel::clock_tag::audio
            && prog.clock_of(crossing.to) == kernel::clock_tag::ui)
        {
            REQUIRE(crossing.policy == conc::policy::latest);
            back = true;
        }

    REQUIRE(back);
}

TEST_CASE("daw | Owning and borrowing programs agree on struct wires", "[daw]")
{
    // Parity across program constructions with user values on the wires: the
    // borrowed program materializes its seeds into its own pools and reads
    // the same cells the same way.
    const auto recipe { daw::net::app(kScore) };
    kernel::program owned { recipe };

    const auto net { shell::compile(recipe) };
    kernel::program borrowed { net.descriptor() };

    kernel::release_engine e1;
    kernel::release_engine e2;

    set_ui(owned);
    set_ui(borrowed);
    e1.step(owned, kernel::clock_tag::ui);
    e2.step(borrowed, kernel::clock_tag::ui);

    for (std::int64_t k { 0 }; k < 3; ++k)
    {
        const auto r { run_block(owned, e1, k * kBlock) };
        const auto b { run_block(borrowed, e2, k * kBlock) };

        REQUIRE(b.playhead == Catch::Approx(r.playhead));
        REQUIRE(b.midi_count == r.midi_count);
        REQUIRE(b.midi_offset == r.midi_offset);
        REQUIRE(b.midi_off_count == r.midi_off_count);
        REQUIRE(b.voice_gain == Catch::Approx(r.voice_gain));

        // Both sides tick ui after audio, so the back-crossed playhead is
        // part of the parity invariant too.
        e1.step(owned, kernel::clock_tag::ui);
        e2.step(borrowed, kernel::clock_tag::ui);

        REQUIRE(
            get<double>(borrowed, "playhead_ui")
            == Catch::Approx(get<double>(owned, "playhead_ui")));
    }
}

TEST_CASE("daw | Programs register DAW struct schemas", "[daw]")
{
    // Every wire struct ships its debugger schema (TransportIn::seek
    // excepted: an optional member has no field kind), and the nested
    // bundle fields resolve to the leaf schemas.
    kernel::program prog { daw::net::app(kScore) };

    const auto find = [&](std::string_view name)
    {
        for (const auto* type : prog.types())
            if (type->name != nullptr && std::string_view { type->name } == name)
                return type;

        return static_cast<const kernel::user_type*>(nullptr);
    };

    const auto* block { find("daw.block_desc") };
    const auto* loop { find("daw.loop_region") };
    const auto* pos { find("daw.pos_pair") };
    const auto* voice { find("daw.voice") };
    const auto* transport { find("daw.transport_in") };

    REQUIRE(block != nullptr);
    REQUIRE(loop != nullptr);
    REQUIRE(pos != nullptr);
    REQUIRE(voice != nullptr);
    REQUIRE(transport != nullptr);

    // Distinct ids: a collision would be a naming bug, caught loudly.
    const std::uint32_t ids[] { block->id, loop->id, pos->id, voice->id, transport->id };

    for (const auto id : ids)
        REQUIRE(id != 0);

    for (std::size_t i { 0 }; i < 5; ++i)
        for (std::size_t j { i + 1 }; j < 5; ++j)
            REQUIRE(ids[i] != ids[j]);

    REQUIRE(block->fields.size() == 3);
    REQUIRE(loop->fields.size() == 3);
    REQUIRE(transport->fields.size() == 5);

    const kernel::user_type* block_nested { nullptr };
    const kernel::user_type* loop_nested { nullptr };

    for (const auto& field : transport->fields)
    {
        if (field.name != nullptr && std::string_view { field.name } == "block")
            block_nested = field.nested;
        if (field.name != nullptr && std::string_view { field.name } == "loop")
            loop_nested = field.nested;
    }

    REQUIRE(block_nested == block);
    REQUIRE(loop_nested == loop);

    // The optional seek reads engaged-or-absent; the flag is what tells the
    // reader apart from a plain double.
    bool seek_optional { false };
    bool seek_found { false };

    for (const auto& field : transport->fields)
    {
        if (field.name != nullptr && std::string_view { field.name } == "seek")
        {
            seek_found = true;
            seek_optional = field.optional;
        }
    }

    REQUIRE(seek_found);
    REQUIRE(seek_optional);
}

TEST_CASE("daw | Shared construction matches the DAWmulator", "[daw]")
{
    // The debugger's `daw` example builds through daw::dawmulator_recipe +
    // daw::init_dawmulator and then presses play (tools/web-debugger/main.cpp);
    // the web shell builds the same recipe and inits the same levels through
    // Dawmulator::initDefaults (demo/daw/shell). Pin the shared construction
    // to the §6.1 values under a debug engine: with L5 parity (pinned above
    // on this network) that is the DAWmulator's program in every shell.
    kernel::program prog { daw::dawmulator_recipe() };

    std::string trace;
    kernel::text_trace_sink sink { trace };
    kernel::debug_engine eng { sink };

    daw::init_dawmulator(prog, eng);

    // Press play: the level publishes on the next ui tick (a latest link
    // only carries what the producer ticked), the way the debugger's ui
    // loop picks the set up on its next iteration.
    prog.set("playing", true);
    eng.step(prog, kernel::clock_tag::ui);

    const auto b0 { run_block(prog, eng, 0) };
    const auto b1 { run_block(prog, eng, kBlock) };
    const auto b2 { run_block(prog, eng, 2 * kBlock) };
    const auto b3 { run_block(prog, eng, 3 * kBlock) };
    const auto b4 { run_block(prog, eng, 4 * kBlock) };

    REQUIRE(b0.playhead == Catch::Approx(1.0));
    REQUIRE(b1.playhead == Catch::Approx(2.0));
    REQUIRE(b2.playhead == Catch::Approx(3.0));
    REQUIRE(b3.playhead == Catch::Approx(0.0));
    REQUIRE(b4.playhead == Catch::Approx(1.0));

    REQUIRE(b3.midi_count == 1);
    REQUIRE(b3.midi_offset == 12000);
    REQUIRE(b0.midi_off_count == 1);
    REQUIRE(b4.midi_off_count == 1);
    REQUIRE(b3.voice_gain == Catch::Approx(0.8));
    REQUIRE(b4.voice_gain == Catch::Approx(0.0));

    eng.step(prog, kernel::clock_tag::ui);
    REQUIRE(get<double>(prog, "playhead_ui") == Catch::Approx(1.0));
}
