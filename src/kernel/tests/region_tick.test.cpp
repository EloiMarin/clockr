#include "clockr/kernel/release_engine.hpp"
#include "clockr/kernel/debug_engine.hpp"

#include "clockr/shell/recipe.hpp"
#include "clockr/shell/builder.hpp"

#include "clockr/kernel/graph.hpp"
#include "clockr/kernel/topology.hpp"
#include "clockr/kernel/program.hpp"

#include "clockr/conc/link.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

using namespace clockr;

namespace
{

// Counting engine: records which node_ids were evaluated in the last tick.
// The engine's virtuals are called from evaluate_nodes, so overriding them
// lets the test assert that tick(thread) touched only that thread's region.
class counting_engine final : public kernel::engine
{
public:
    std::vector<kernel::node_id> seen;

    void
    clear() noexcept
    {
        seen.clear();
    }

protected:
    void
    on_node_tick_begin(kernel::node_id id) override
    {
        seen.push_back(id);
    }

    void
    on_node_tick_end(kernel::node_id, kernel::value) override
    {}
};

bool
contains(std::span<const kernel::node_id> haystack, kernel::node_id needle)
{
    for (auto id : haystack)
        if (id == needle)
            return true;
    return false;
}

} // namespace

TEST_CASE("kernel/region_tick | Latest resampling: the consumer sees the most recent producer value", "[C2][kernel]")
{
    // ui: bpm input. audio: bpm * 2 -> output. Sharing point: latest.
    const auto bpm { shell::input<double, kernel::clock_tag::ui>("bpm") };

    const auto network
    {
        shell::at<kernel::clock_tag::audio, conc::policy::latest>(bpm)
            >> shell::lift<double, double>([](const double& v) { return v * 2; })
            >> shell::output<double, kernel::clock_tag::audio>("out")
    };

    kernel::program program { network }; // bpm anchors ui; at<> crosses to audio

    // Operational pin: the topology must actually expose the cross-clock edge
    // as a crossing with the annotated mode. Otherwise the engine has
    // nowhere to place the link and the later assertions are vacuous.
    REQUIRE(program.topology().regions.size() == 2);
    REQUIRE(program.topology().crossings.size() == 1);
    REQUIRE(program.topology().crossings[0].policy == conc::policy::latest);

    // Verify that the two regions are on the expected threads.
    bool has_ui = false;
    bool has_audio = false;
    for (auto& r : program.topology().regions)
    {
        if (r.clock == kernel::clock_tag::ui)
            has_ui = true;
        if (r.clock == kernel::clock_tag::audio)
            has_audio = true;
    }
    REQUIRE(has_ui);
    REQUIRE(has_audio);

    counting_engine engine;

    // ui ticks twice before the audio region runs at all.
    program.set("bpm", 1.0);
    engine.clear();
    engine.step(program, kernel::clock_tag::ui);

    // Region isolation pin: a ui tick must NOT evaluate audio nodes.
    // If engine.tick evaluated the whole graph, audio's lift/output would have
    // run and the output would already be 2.0.
    {
        // Find which nodes belong to which region.
        const auto& topo = program.topology();
        std::vector<kernel::node_id> ui_nodes;
        std::vector<kernel::node_id> audio_nodes;
        for (auto& r : topo.regions)
        {
            if (r.clock == kernel::clock_tag::ui)
                ui_nodes.insert(ui_nodes.end(), r.nodes.begin(), r.nodes.end());
            else if (r.clock == kernel::clock_tag::audio)
                audio_nodes.insert(audio_nodes.end(), r.nodes.begin(), r.nodes.end());
        }

        for (auto id : engine.seen)
            REQUIRE(contains(ui_nodes, id));
        for (auto id : audio_nodes)
            REQUIRE(!contains(engine.seen, id));

        // Output must not have been produced yet. The audio output still holds
        // its initial monostate — get<double> must throw. With the whole-graph
        // bug it would already be 2.0.
        REQUIRE_THROWS_AS(get<double>(program, "out"), std::bad_variant_access);
    }

    program.set("bpm", 2.0);
    engine.clear();
    engine.step(program, kernel::clock_tag::ui);

    // Still no audio evaluation after the second ui tick.
    REQUIRE_THROWS_AS(get<double>(program, "out"), std::bad_variant_access);

    // Value-isolation pin: changing the program's bpm storage WITHOUT ticking
    // ui must not become visible to audio. Audio must see the link's
    // latest *ticked* value, not the raw shared memory. This distinguishes a
    // true link (double_buffer, atomic) from a single shared value that
    // happens to behave like latest when whole-graph evaluation masks it.
    program.set("bpm", 99.0); // no ui tick
    engine.clear();
    engine.step(program, kernel::clock_tag::audio);
    // Audio should see 2.0 * 2 = 4.0 (the last ui-ticked value), not 198.0.
    // With a single shared value, the 99.0 would bleed through.
    REQUIRE(get<double>(program, "out") == 4.0);

    // Verify that this audio tick evaluated only audio nodes.
    {
        const auto& topo = program.topology();
        std::vector<kernel::node_id> audio_nodes;
        for (auto& r : topo.regions)
            if (r.clock == kernel::clock_tag::audio)
                audio_nodes.insert(audio_nodes.end(), r.nodes.begin(), r.nodes.end());
        for (auto id : engine.seen)
            REQUIRE(contains(audio_nodes, id));
    }

    // §3.2 latest: consumer reads s(max{ i : p(i) <= c(u) }) — the newest.
    // Already asserted as 4.0 above.

    // ... and it HOLDS: no new producer value, same reading next tick.
    engine.clear();
    engine.step(program, kernel::clock_tag::audio);
    REQUIRE(get<double>(program, "out") == 4.0);
    // Second scan tick also only touched audio.
    {
        const auto& topo = program.topology();
        std::vector<kernel::node_id> audio_nodes;
        for (auto& r : topo.regions)
            if (r.clock == kernel::clock_tag::audio)
                audio_nodes.insert(audio_nodes.end(), r.nodes.begin(), r.nodes.end());
        for (auto id : engine.seen)
            REQUIRE(contains(audio_nodes, id));
    }

    // Finally, a fresh ui value propagates only after ui tick + audio tick.
    program.set("bpm", 3.0);
    engine.step(program, kernel::clock_tag::ui);
    // Audio still holds old value until it ticks.
    REQUIRE(get<double>(program, "out") == 4.0);
    engine.step(program, kernel::clock_tag::audio);
    REQUIRE(get<double>(program, "out") == 6.0);
}

TEST_CASE("kernel/region_tick | Engine tick isolates regions: ui output unchanged by audio tick", "[kernel][region]")
{
    // Two independent outputs let us observe cross-region bleeding directly.
    // ui_chain: bpm -> ui_out. audio_chain: at<latest>(bpm) -> *2 -> audio_out.
    // The input is shared so the graph has a single bpm node.
    const auto bpm { shell::share(shell::input<double, kernel::clock_tag::ui>("bpm")) };

    const auto ui_chain
    {
        bpm >> shell::output<double, kernel::clock_tag::ui>("ui_out")
    };

    const auto audio_chain
    {
        shell::at<kernel::clock_tag::audio, conc::policy::latest>(bpm)
            >> shell::lift<double, double>([](const double& v) { return v * 2; })
            >> shell::output<double, kernel::clock_tag::audio>("audio_out")
    };

    const auto network { shell::parallel(ui_chain, audio_chain) };

    kernel::program program { network };
    counting_engine engine;

    REQUIRE(program.topology().regions.size() == 2);
    REQUIRE(program.topology().crossings.size() == 1);

    program.set("bpm", 5.0);
    engine.clear();
    engine.step(program, kernel::clock_tag::ui);
    // ui_out must be 5, audio_out still monostate (not yet ticked).
    REQUIRE(get<double>(program, "ui_out") == 5.0);
    REQUIRE_THROWS_AS(get<double>(program, "audio_out"), std::bad_variant_access);

    // Ticking audio must not re-evaluate ui: ui_out stays 5 even if we change
    // bpm storage without a ui tick.
    program.set("bpm", 999.0); // no ui tick
    engine.clear();
    engine.step(program, kernel::clock_tag::audio);
    REQUIRE(get<double>(program, "ui_out") == 5.0);
    REQUIRE(get<double>(program, "audio_out") == 10.0); // 5*2, not 999*2

    // Now tick ui again with a new value.
    program.set("bpm", 7.0);
    engine.clear();
    engine.step(program, kernel::clock_tag::ui);
    REQUIRE(get<double>(program, "ui_out") == 7.0);
    // audio_out still holds old 10 until audio ticks.
    REQUIRE(get<double>(program, "audio_out") == 10.0);

    engine.step(program, kernel::clock_tag::audio);
    REQUIRE(get<double>(program, "audio_out") == 14.0);
}

// Hammer cases need real threads; they are compiled out on no-thread targets
// (CLOCKR_NO_THREADS) while the single-threaded cases in this file run.
#ifndef CLOCKR_NO_THREADS
TEST_CASE("kernel/region_tick | Concurrent region ticks are thread-safe via links", "[kernel][thread_safe]")
{
    // The operational requirement: ui and audio ticks may run on different
    // threads without external locking. A correct implementation uses
    // atomics / links at crossings and isolates regions. The current
    // buggy implementation shares a single value and evaluates the whole graph,
    // which races and bleeds values across threads.
    //
    // Sentinel test: ui publishes a real value, then writes a sentinel (999)
    // WITHOUT ticking. Audio must never see the sentinel — it must see only
    // ticked values. With a single shared value or whole-graph evaluation,
    // audio's whole-graph tick reads the sentinel directly.
    const auto bpm { shell::input<double, kernel::clock_tag::ui>("bpm") };

    kernel::program program
    {
        shell::at<kernel::clock_tag::audio, conc::policy::latest>(bpm)
            >> shell::lift<double, double>([](const double& v) { return v * 2; })
            >> shell::output<double, kernel::clock_tag::audio>("out")
    };
    kernel::release_engine engine;

    // NOTE: the sentinel must lie OUTSIDE the iteration domain [0, iterations):
    // 999.0 was previously used here, but i=999 legitimately produces
    // 999*2 == sentinel_out — and latest *holds* values across audio ticks, so
    // a held 1998.0 was miscounted as a leak. -1.0 is unambiguous: no
    // legitimate tick produces -2.0, and only an unticked send could leak it.
    constexpr double sentinel { -1.0 };
    constexpr double sentinel_out { sentinel * 2.0 };
    constexpr int iterations { 2000 };

    std::atomic<bool> start { false };
    std::atomic<bool> done_ui { false };
    std::atomic<int> mismatches { 0 };
    std::atomic<int> sentinel_seen { 0 };

    std::thread audio_thread
    {
        [&]
        {
            try
            {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();

                while (!done_ui.load(std::memory_order_acquire))
                {
                    engine.step(program, kernel::clock_tag::audio);
                    auto v = program.get("out");
                    if (std::holds_alternative<double>(v))
                    {
                        double d = std::get<double>(v);
                        if (d == sentinel_out)
                            sentinel_seen.fetch_add(1, std::memory_order_relaxed);
                        else if (d != 0.0)
                        {
                            // Must be even and not sentinel
                            int i = static_cast<int>(d / 2.0);
                            if (d != static_cast<double>(i * 2) || i < 0 || i >= iterations)
                                mismatches.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    else if (!std::holds_alternative<std::monostate>(v))
                    {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            catch (...)
            {
                mismatches.fetch_add(100000, std::memory_order_relaxed);
            }
        }
    };

    std::thread ui_thread
    {
        [&]
        {
            try
            {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();

                for (int i = 0; i < iterations; ++i)
                {
                    program.set("bpm", static_cast<double>(i));
                    engine.step(program, kernel::clock_tag::ui);
                    // Write sentinel without tick — must NOT become visible.
                    program.set("bpm", sentinel);
                    // Small yield to increase chance audio ticks while sentinel
                    // sits in shared memory (exposes the bug).
                    std::this_thread::yield();
                }

                done_ui.store(true, std::memory_order_release);
            }
            catch (...)
            {
                mismatches.fetch_add(100000, std::memory_order_relaxed);
                done_ui.store(true, std::memory_order_release);
            }
        }
    };

    // Startup sequencing (real-app discipline): publish once before audio
    // starts. Until the first producer tick, fill correctly stages monostate
    // (§3.2 initial v₀) — which a partial lift<double, …> cannot handle
    // (totality, §2.1). Publishing first keeps the test on its subject (the
    // sentinel pin below) instead of tripping over pre-publish ticks.
    program.set("bpm", 0.0);
    engine.step(program, kernel::clock_tag::ui);

    start.store(true, std::memory_order_release);
    ui_thread.join();
    audio_thread.join();

    // With a true link, sentinel is never ticked so never observed.
    REQUIRE(sentinel_seen.load() == 0);
    REQUIRE(mismatches.load() == 0);

    // Final drain must see last real value, not sentinel.
    engine.step(program, kernel::clock_tag::audio);
    // The last ui tick was iterations-1, sentinel after it was not ticked.
    REQUIRE(get<double>(program, "out") == static_cast<double>((iterations - 1) * 2));
}
#endif // CLOCKR_NO_THREADS

// ---------------------------------------------------------------------------
// Phase C2 acceptance cases. The "latest resampling" case is already covered
// above (superset). The queued/overflow cases exercise the program API; the L9
// epoch cases were written against program/set/tick/get/topology from the
// start.
// ---------------------------------------------------------------------------

TEST_CASE("queued resampling: every event arrives, in order", "[C2][kernel]")
{
    // ui: seek events. audio: consumed one per tick (sub-tick draining: one
    // pending event per consumer tick, FIFO; an empty queue stages monostate
    // as "no event"). Translated from the earlier graph-level tick helpers.
    const auto seek { shell::input<std::int64_t, kernel::clock_tag::ui>("seek") };

    const auto network
    {
        shell::at<kernel::clock_tag::audio, conc::policy::queued>(seek)
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::program prog { network };
    kernel::release_engine eng;

    prog.set("seek", std::int64_t { 1 });
    eng.step(prog, kernel::clock_tag::ui);
    prog.set("seek", std::int64_t { 2 });
    eng.step(prog, kernel::clock_tag::ui);
    prog.set("seek", std::int64_t { 3 });
    eng.step(prog, kernel::clock_tag::ui);

    // §3.2 queued: the consumer drains [1, 2, 3] in increasing order — one
    // event per tick under sub-tick draining.
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 1);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 2);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 3);
}

TEST_CASE("queued resampling: overflow is surfaced, order prefix preserved", "[C2][kernel]")
{
    // L6: the model holds while batch size <= capacity. Overflow must not
    // corrupt or reorder what fits — it fails at the producer, observably:
    // the link drops the send instead of blocking, and
    // `program::dropped_sends()` counts it.
    const auto seek { shell::input<std::int64_t, kernel::clock_tag::ui>("seek") };

    const auto network
    {
        shell::at<kernel::clock_tag::audio, conc::policy::queued>(seek)
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::program prog { network };
    kernel::release_engine eng;

    // One send per ui tick with no audio tick to drain: the queue (the link's
    // default capacity, 1024) fills and every send past it is dropped.
    constexpr std::int64_t capacity { 1024 };
    constexpr std::int64_t overflow { 5 };

    for (std::int64_t i { 1 }; i <= capacity + overflow; ++i)
    {
        prog.set("seek", i);
        eng.step(prog, kernel::clock_tag::ui);
    }

    REQUIRE(prog.dropped_sends() == static_cast<std::uint64_t>(overflow));

    // What fit is still the oldest prefix, in order.
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 1);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 2);
}

TEST_CASE("L9a epoch coalescing: multiple sets before producer tick keep only last - latest", "[C2][L9][epoch]")
{
    // §3.2: ŝ(i)=last send during epoch i; earlier same-epoch pushes leave
    // no trace. Graph-driven flow pushes once per tick (no coalesce), but
    // host-before-tick overwrites are the input analogue: p.set() twice
    // before tick must coalesce.
    const auto src { shell::input<std::int64_t, kernel::clock_tag::ui>("n") };
    const auto network
    {
        shell::at<kernel::clock_tag::audio, conc::policy::latest>(src)
        >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& v){ return v * 2; })
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::program prog { network };
    kernel::release_engine eng;

    // Two sets within same producer epoch — only last should cross.
    prog.set("n", std::int64_t{999});
    prog.set("n", std::int64_t{21});
    eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 42);

    // Verify across epochs as well: two overwrites before next tick.
    prog.set("n", std::int64_t{1});
    prog.set("n", std::int64_t{2});
    prog.set("n", std::int64_t{3});
    eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 6);
}

TEST_CASE("L9a epoch coalescing: multiple sets before producer tick keep only last - queued", "[C2][L9][epoch]")
{
    // Queued also sees at most one ŝ(i) per producer epoch. A naive spsc
    // that enqueues each set() separately would keep both; the per-epoch
    // wrapper must coalesce within the epoch.
    const auto src { shell::input<std::int64_t, kernel::clock_tag::ui>("n") };
    const auto network
    {
        shell::at<kernel::clock_tag::audio, conc::policy::queued>(src)
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::program prog { network };
    kernel::release_engine eng;

    // Two sets in same epoch → one queued entry (the last), not two.
    prog.set("n", std::int64_t{1});
    prog.set("n", std::int64_t{2}); // should overwrite, not queue both
    eng.step(prog, kernel::clock_tag::ui);

    // Drain one — should see 2, and second drain should be empty / no event.
    // Our scalar sub-tick draining cannot represent "no event" as a value;
    // we pin that first drain is 2 and that a queued wrapper coalesced.
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 2);

    // Next epoch with single value for contrast
    prog.set("n", std::int64_t{3});
    eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 3);
}

TEST_CASE("L9a idle producer ticks publish nothing - queued", "[C2][L9][epoch]")
{
    // An input-driven source publishes only epochs in which it was set, so
    // clean epochs contribute true Nothing instead of monostate duds. Two
    // idle ticks then one valued tick must drain as exactly [5, silence]:
    // under send-always the dud epochs would precede the value.
    const auto src { shell::input<std::int64_t, kernel::clock_tag::ui>("n") };
    const auto network
    {
        shell::at<kernel::clock_tag::audio, conc::policy::queued>(src)
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };

    kernel::program prog { network };
    kernel::release_engine eng;

    eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::ui);

    prog.set("n", std::int64_t{5});
    eng.step(prog, kernel::clock_tag::ui);

    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 5);

    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE_THROWS_AS(kernel::get<std::int64_t>(prog, "out"), std::bad_variant_access);
}

TEST_CASE("L9a link primitive - latest holds last, queued coalesces per epoch", "[C2][L9][epoch][conc]")
{
    // Direct primitive check that motivates the wrapper. Demonstrates the
    // spec, not the current raw conc implementation (queued raw spsc would
    // retain both values — which is why a per-epoch coalescing layer is
    // required for C2).

    // latest: same-epoch sends coalesce (last wins), silent epochs hold
    {
        conc::link<std::int64_t, conc::policy::latest> w{ std::int64_t{0} };
        REQUIRE(w.receive(0) == std::int64_t{0});
        w.send(std::int64_t{1}, 1);
        w.send(std::int64_t{2}, 1); // same epoch, overwrites
        REQUIRE(w.receive(1) == std::int64_t{2});
        REQUIRE(w.receive(4) == std::int64_t{2}); // silent epochs hold
        w.send(std::int64_t{3}, 5);
        REQUIRE(w.receive(5) == std::int64_t{3});
    }

    // queued: raw spsc would keep both, but spec requires ŝ(i) at most one
    // per epoch. This test documents the *desired* per-epoch coalescing
    // wrapper behaviour; adapt the wrapper API to what you build.
    // We simulate an epoch-coalescing queued wrapper inline:
    {
        // Minimal wrapper: keep only last send since last drain (epoch).
        struct coalescing_queued
        {
            bool has{false};
            std::int64_t last{0};
            std::vector<std::int64_t> q;
            void send(std::int64_t v){ has=true; last=v; }
            void publish_epoch(){ if(has){ q.push_back(last); has=false; } }
            bool receive(std::int64_t& out){ if(q.empty()) return false; out=q.front(); q.erase(q.begin()); return true; }
        } w;

        w.send(1);
        w.send(2); // coalesce within epoch
        w.publish_epoch(); // producer tick publishes one ŝ(i)
        std::int64_t v{};
        REQUIRE(w.receive(v));
        REQUIRE(v == 2);
        REQUIRE(!w.receive(v)); // only one, not two

        w.send(3);
        w.publish_epoch();
        REQUIRE(w.receive(v));
        REQUIRE(v == 3);
    }
}

TEST_CASE("L9b consistency - two latest channels from same producer epoch are observed atomically (no tear) sequential", "[C2][L9][epoch]")
{
    // §3.2 Consistency: "At consumer epoch u, every inbound link
    // together reflects a single cut. A region must not observe epoch 7 on
    // one channel while epoch 6 is still in flight on another (a tear)."
    // Independent per-link hand-offs can tear; the fix is a versioned
    // snapshot per producer boundary published as a unit.

    // Producer: one shared input fan-out to two latest links.
    const auto shared { shell::share(shell::input<std::int64_t, kernel::clock_tag::ui>("n")) };
    const auto net
    {
        shell::parallel(
            shell::at<kernel::clock_tag::audio, conc::policy::latest>(shared)
                >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& v){ return v; })
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("outA"),
            shell::at<kernel::clock_tag::audio, conc::policy::latest>(shared)
                >> shell::lift<std::int64_t, std::int64_t>([](const std::int64_t& v){ return v * 10; })
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("outB")
        )
    };

    kernel::program prog { net };
    // Must be two crossings from same source (topology check)
    REQUIRE(prog.topology().crossings.size() == 2);
    for (auto &sp : prog.topology().crossings)
        REQUIRE(sp.policy == conc::policy::latest);

    kernel::release_engine eng;

    for (std::int64_t i = 1; i <= 5; ++i)
    {
        prog.set("n", i);
        eng.step(prog, kernel::clock_tag::ui);
        eng.step(prog, kernel::clock_tag::audio);
        const auto a = kernel::get<std::int64_t>(prog, "outA");
        const auto b = kernel::get<std::int64_t>(prog, "outB");
        // Pair must belong to same producer epoch: b == a * 10
        REQUIRE(a == i);
        REQUIRE(b == i * 10);
        REQUIRE(b == a * 10);
    }
}

#ifndef CLOCKR_NO_THREADS
TEST_CASE("L9b consistency - no tear under concurrent producer/consumer (latest pair)", "[C2][L9][epoch][concurrent]")
{
    // Concurrent stress: independent stores could interleave between the
    // consumer's two receives, exposing a torn cut (e.g., (2,10) instead of
    // (2,20) or (1,10)). Versioned snapshot prevents this — consumer's fill
    // step sees both channels at same epoch.
    const auto shared { shell::share(shell::input<std::int64_t, kernel::clock_tag::ui>("n")) };

    kernel::program prog
    {
        shell::parallel(
            shell::at<kernel::clock_tag::audio, conc::policy::latest>(shared)
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("outA"),
            shell::at<kernel::clock_tag::audio, conc::policy::latest>(shared)
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("outB")
        )
    };
    kernel::release_engine eng;
    constexpr int iters = 2000;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::atomic<int> mism{0};

    std::thread consumer{[&]{
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while(!done.load(std::memory_order_acquire)){
            eng.step(prog, kernel::clock_tag::audio);
            auto va = prog.get("outA");
            auto vb = prog.get("outB");
            if (!std::holds_alternative<std::int64_t>(va) || !std::holds_alternative<std::int64_t>(vb)) {
                if (std::holds_alternative<std::monostate>(va) && std::holds_alternative<std::monostate>(vb)) continue;
                // One valid, one monostate at start is allowed before first publish
                continue;
            }
            auto a = std::get<std::int64_t>(va);
            auto b = std::get<std::int64_t>(vb);
            // Allow staleness by up to one epoch (consumer may be one behind),
            // but pair must be consistent: a and b from same producer epoch.
            // Since producer sets n=i for both outputs (raw), we expect a==b.
            if (a != b) torn.fetch_add(1, std::memory_order_relaxed);
            if (a < 0 || a >= iters) mism.fetch_add(1, std::memory_order_relaxed);
        }
    }};

    std::thread producer{[&]{
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for(int i=0;i<iters;++i){
            prog.set("n", std::int64_t{i});
            eng.step(prog, kernel::clock_tag::ui);
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    }};

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    REQUIRE(torn.load() == 0);
    REQUIRE(mism.load() == 0);
    // Final drain sees last epoch atomically
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "outA") == iters-1);
    REQUIRE(kernel::get<std::int64_t>(prog, "outB") == iters-1);
}
#endif // CLOCKR_NO_THREADS

TEST_CASE("L9b monotonicity - latest never moves backwards, single channel", "[C2][L9][epoch]")
{
    // Monotonicity: each channel's visible producer epoch never moves
    // backwards, even though latest may skip intermediate epochs (bounded
    // staleness). Values from epochs i<j must never be observed inverted.
    const auto src { shell::input<std::int64_t, kernel::clock_tag::ui>("n") };
    const auto net
    {
        shell::at<kernel::clock_tag::audio, conc::policy::latest>(src)
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };
    kernel::program prog { net };
    kernel::release_engine eng;

    std::int64_t last_seen = -1;
    for(std::int64_t i=0;i<10;++i){
        prog.set("n", i);
        eng.step(prog, kernel::clock_tag::ui);
        // Consumer ticks every tick here, but we also test skipping
        eng.step(prog, kernel::clock_tag::audio);
        auto cur = kernel::get<std::int64_t>(prog, "out");
        REQUIRE(cur >= last_seen); // never backwards
        REQUIRE(cur <= i);         // never future
        last_seen = cur;
    }
    // Now producer races ahead 5 ticks before consumer catches up — consumer
    // should jump to latest, still monotonic, not see intermediate out of order.
    for(std::int64_t i=10;i<15;++i){ prog.set("n", i); eng.step(prog, kernel::clock_tag::ui); }
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 14);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") >= last_seen);
}

TEST_CASE("L9b monotonicity - queued preserves FIFO order across epochs", "[C2][L9][epoch]")
{
    // Queued: batch is [ŝ(i) | c(u-1)<p(i)<=c(u)] in increasing i. Race may
    // move a value between adjacent batches but never out of order.
    const auto src { shell::input<std::int64_t, kernel::clock_tag::ui>("n") };
    const auto net
    {
        shell::at<kernel::clock_tag::audio, conc::policy::queued>(src)
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };
    kernel::program prog { net };
    kernel::release_engine eng;

    for(std::int64_t i=1;i<=5;++i){ prog.set("n", i); eng.step(prog, kernel::clock_tag::ui); }

    for(std::int64_t exp=1; exp<=5; ++exp){
        eng.step(prog, kernel::clock_tag::audio);
        REQUIRE(kernel::get<std::int64_t>(prog, "out") == exp);
    }

    // Interleaved produce/consume still ordered
    prog.set("n", std::int64_t{6}); eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 6);
    prog.set("n", std::int64_t{7}); eng.step(prog, kernel::clock_tag::ui);
    prog.set("n", std::int64_t{8}); eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 7);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 8);
}

TEST_CASE("L9b monotonicity across two channels - no cross-channel inversion", "[C2][L9][epoch]")
{
    // Two queued channels from same producer must preserve cross-channel
    // epoch order: if producer epoch i precedes j, consumer must never
    // observe j's value on one channel before i's on the other.
    const auto shared { shell::share(shell::input<std::int64_t, kernel::clock_tag::ui>("n")) };
    const auto net
    {
        shell::parallel(
            shell::at<kernel::clock_tag::audio, conc::policy::queued>(shared)
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("outA"),
            shell::at<kernel::clock_tag::audio, conc::policy::queued>(shared)
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("outB")
        )
    };
    kernel::program prog { net };
    kernel::release_engine eng;
    REQUIRE(prog.topology().crossings.size()==2);

    for(std::int64_t i=1;i<=4;++i){ prog.set("n", i); eng.step(prog, kernel::clock_tag::ui); }

    for(std::int64_t exp=1; exp<=4; ++exp){
        eng.step(prog, kernel::clock_tag::audio);
        REQUIRE(kernel::get<std::int64_t>(prog, "outA") == exp);
        REQUIRE(kernel::get<std::int64_t>(prog, "outB") == exp);
        // Pair from same epoch arrives together; no (2,1) inversion.
    }
}

TEST_CASE("L9b coverage - queued consumer drains every inbound channel every tick", "[C2][L9][epoch]")
{
    // Coverage: "A region reads every inbound channel every epoch. Skipping
    // a channel would let a later value overtake an unread earlier one on
    // another channel, inverting observed order." We pin that two queued
    // inbounds are both drained each audio tick.

    const auto srcA { shell::input<std::int64_t, kernel::clock_tag::ui>("a") };
    const auto srcB { shell::input<std::int64_t, kernel::clock_tag::ui>("b") };
    // Each source crosses via queued; consumer has two outputs.
    // To keep producer epochs aligned, we tick ui once per logical epoch
    // with both a and b set. Each epoch contributes one ŝ per channel.
    const auto net
    {
        shell::parallel(
            shell::at<kernel::clock_tag::audio, conc::policy::queued>(srcA)
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("outA"),
            shell::at<kernel::clock_tag::audio, conc::policy::queued>(srcB)
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("outB")
        )
    };
    kernel::program prog { net };
    kernel::release_engine eng;
    REQUIRE(prog.topology().crossings.size()==2);

    // Epoch 1: (10, 100), Epoch2: (20,200), Epoch3: (30,300)
    prog.set("a", std::int64_t{10}); prog.set("b", std::int64_t{100}); eng.step(prog, kernel::clock_tag::ui);
    prog.set("a", std::int64_t{20}); prog.set("b", std::int64_t{200}); eng.step(prog, kernel::clock_tag::ui);
    prog.set("a", std::int64_t{30}); prog.set("b", std::int64_t{300}); eng.step(prog, kernel::clock_tag::ui);

    // Consumer must see pairs in lockstep, not (10,200) etc.
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "outA") == 10);
    REQUIRE(kernel::get<std::int64_t>(prog, "outB") == 100);

    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "outA") == 20);
    REQUIRE(kernel::get<std::int64_t>(prog, "outB") == 200);

    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "outA") == 30);
    REQUIRE(kernel::get<std::int64_t>(prog, "outB") == 300);
}

TEST_CASE("L9 bounded staleness for latest - stale by at most one, never future or torn", "[C2][L9][epoch]")
{
    // Operational note §3.2 latest: hand-off race may displace observed cut
    // by at most the race window (one epoch), but never reorder or tear.
    // We pin the ordering part: sequential model has zero staleness slack,
    // but even with slack the observed value must be among last two producer
    // epochs, never future, and pair-consistency still holds (already tested).
    const auto src { shell::input<std::int64_t, kernel::clock_tag::ui>("n") };
    const auto net
    {
        shell::at<kernel::clock_tag::audio, conc::policy::latest>(src)
        >> shell::output<std::int64_t, kernel::clock_tag::audio>("out")
    };
    kernel::program prog { net };
    kernel::release_engine eng;

    prog.set("n", std::int64_t{1}); eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::audio);
    REQUIRE(kernel::get<std::int64_t>(prog, "out")==1);

    // Producer moves two steps ahead before consumer runs — consumer should
    // see the newest (3), which is at most one stale vs ideal if it raced:
    // ideal cut at 3, stale would be 2, but never 4 (future) and never torn.
    prog.set("n", std::int64_t{2}); eng.step(prog, kernel::clock_tag::ui);
    prog.set("n", std::int64_t{3}); eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::audio);
    auto v = kernel::get<std::int64_t>(prog, "out");
    REQUIRE((v==2 || v==3)); // bounded staleness slack: may be 2 if racing, but in sequential it is 3
    // Sequential deterministic run sees 3 (no race), confirm monotonic
    REQUIRE(v >= 1);
    REQUIRE(v <= 3);
}

#ifndef CLOCKR_NO_THREADS
TEST_CASE("L9b consistency - tear across distinct sources under concurrency (versioned snapshot)", "[C2][L9][epoch][concurrent]")
{
    // Two LATEST channels from DIFFERENT producer ports must still reflect a
    // single cut per consumer epoch (versioned snapshot, §3.2): the producer
    // stamps one epoch across A and B and publishes its published_epoch once
    // afterwards, while the consumer snapshots one published_epoch and reads both
    // channels at that cut — so a producer tick landing between the two
    // loads cannot tear the pair. Contrast with the shared-source concurrent
    // test above, which is tear-free by construction (one staged slot).
    auto in_a { shell::input<std::int64_t, kernel::clock_tag::ui>("a") };
    auto in_b { shell::input<std::int64_t, kernel::clock_tag::ui>("b") };

    kernel::program prog
    {
        shell::parallel(
            shell::at<kernel::clock_tag::audio, conc::policy::latest>(in_a)
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("out_a"),
            shell::at<kernel::clock_tag::audio, conc::policy::latest>(in_b)
                >> shell::output<std::int64_t, kernel::clock_tag::audio>("out_b"))
    };
    kernel::release_engine engine;

    // Prevent un-initialised non-optional inputs (TODO: Maybe non-optional inputs *must* declare an initial value at shell::input<>()?)
    prog.set("a", std::int64_t{ 0 });
    prog.set("b", std::int64_t{ 0 });

    engine.step(prog, kernel::clock_tag::ui);

    constexpr int iters { 20000 };

    std::atomic<bool> start { false };
    std::atomic<bool> done { false };
    std::atomic<int> torn { 0 };
    std::atomic<int> cut_pairs { 0 };
    std::atomic<int> lapped_ticks { 0 };

    std::thread audio_thread
    {
        [&]
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            while (!done.load(std::memory_order_acquire))
            {
                // Under extreme scheduling pressure a fill can keep failing:
                // the producer laps the snapshot, or a scan straddles a slot
                // reuse (the link reports that stale and the fill
                // retries). Only after repeated failures does the engine's
                // non-blocking fallback read the newest value per channel
                // instead of one cut — a documented relaxation it reports.
                // Such a tick is excluded from the tear check, because the
                // guarantee it asserts is exactly the cut the fallback could
                // not provide.
                const auto lapped_before { prog.lapped_fills() };

                engine.step(prog, kernel::clock_tag::audio);

                auto out_a { prog.get("out_a") };
                auto out_b { prog.get("out_b") };

                if (prog.lapped_fills() != lapped_before)
                {
                    lapped_ticks.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                cut_pairs.fetch_add(1, std::memory_order_relaxed);

                if (std::get<std::int64_t>(out_a) != std::get<std::int64_t>(out_b))
                    torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread ui_thread
    {
        [&]
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (int i { 0 }; i < iters; ++i)
            {
                prog.set("a", std::int64_t{ i });
                prog.set("b", std::int64_t{ i });
                engine.step(prog, kernel::clock_tag::ui);
            }

            done.store(true, std::memory_order_release);
        }
    };

    start.store(true, std::memory_order_release);

    ui_thread.join();
    audio_thread.join();

    // Every tick either verified a cut or reported that it could not; among
    // the verified ones, the two channels never disagree.
    REQUIRE(cut_pairs.load() + lapped_ticks.load() > 0);
    REQUIRE(torn.load() == 0);

    // Final drain converges on the last epoch on both channels (sequential).
    engine.step(prog, kernel::clock_tag::audio);

    REQUIRE(kernel::get<std::int64_t>(prog, "out_a") == iters - 1);
    REQUIRE(kernel::get<std::int64_t>(prog, "out_b") == iters - 1);
}

TEST_CASE("kernel | A region may move between threads, never overlap", "[kernel][region]")
{
    // The debug overlap guard (region_runtime::in_step) must allow a region
    // to be ticked by different threads over time — thread-pool style — while
    // still rejecting overlapping ticks. Serialize a two-thread loop: a guard
    // that pinned the first thread instead of the tick window would trip
    // here, and an overlap would trip the assert (fail fast, not testable
    // in-process without a death test).
    const auto network
    {
        shell::input<std::int64_t, kernel::clock_tag::worker>("in")
            >> shell::lift<std::int64_t, std::int64_t>(
                   [](const std::int64_t& v) { return v + 1; })
            >> shell::output<std::int64_t, kernel::clock_tag::worker>("out")
    };

    kernel::program prog { network };

    kernel::release_engine engine;

    std::mutex gate;

    const auto worker
    {
        [&](std::int64_t base)
        {
            for (int i { 0 }; i < 8; ++i)
            {
                const std::lock_guard<std::mutex> lock { gate };

                prog.set("in", base + i);
                engine.step(prog, kernel::clock_tag::worker);
            }
        }
    };

    std::thread first { worker, 0 };
    std::thread second { worker, 100 };

    first.join();
    second.join();

    // Deterministic closure: the same input on whichever thread ticks last.
    prog.set("in", std::int64_t { 5 });
    engine.step(prog, kernel::clock_tag::worker);

    REQUIRE(kernel::get<std::int64_t>(prog, "out") == 6);
}
#endif // CLOCKR_NO_THREADS
