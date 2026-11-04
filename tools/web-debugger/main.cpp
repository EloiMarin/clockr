// debugger_demo — runs a small example network on simulated clock threads
// (audio/ui/worker), records every evaluation step, and serves the debug web
// UI over WebSocket.
//
// The network exists to exercise the UI's features, not to make music: all
// three regions, cross-region shares, delay/scan state, a described user
// struct, and a node that throws on demand.
//
//   ./debugger_demo [--host ADDR] [--port N] [--web-root DIR] [--throw-every N]

#include "server/recorder.hpp"
#include "server/server.hpp"

#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/shell/recipe.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

// A user struct the demo sends across clocks. Three 8-byte members are large
// enough for the ref-counted path, so the trace exercises the whole user-value
// lifecycle, not just the inline representation.
struct loop_region
{
    double start { 0.0 };
    double end { 0.0 };
    std::int64_t bars { 0 };
};

namespace clockr::kernel
{
template<>
struct describe<loop_region>
{
    static const user_type&
    type()
    {
        static const std::array fields {
            field("start", &loop_region::start),
            field("end", &loop_region::end),
            field("bars", &loop_region::bars),
        };
        static const user_type t {
            make_user_type<loop_region>("demo.loop_region", fields)
        };

        return t;
    }
};
} // namespace clockr::kernel

namespace
{
using namespace clockr;

// A network with one chain per region, crossed by links, plus state
// and a node that can throw. Deliberately small enough to read at a
// glance in the UI.
auto
demo_network()
{
    using kernel::clock_tag;

    // audio: tempo = bpm * 2, with a scan-based tick counter. `tempo`
    // feeds two consumers (its output and the scan), so it is shared once
    // — a plain fragment used twice would emit its source (and port)
    // again.
    const auto bpm { shell::share(shell::input<double, clock_tag::ui>("bpm")) };
    const auto tempo { shell::share(
        shell::at<clock_tag::audio, conc::policy::latest>(bpm) >>
        shell::lift<double, double>([](const double& v) {
            return v * 2.0;
        })
    ) };
    const auto ticks { tempo >>
                       shell::scan<double, std::int64_t>(
                           0, [](const std::int64_t& previous, const double&) {
                               return previous + 1;
                           }
                       ) };

    // ui: a gain that is mirrored straight through.
    const auto gain { shell::input<double, clock_tag::ui>("gain") };
    const auto gain_out {
        gain >> shell::lift<double, double>([](const double& v) {
            // The demo's throwing node: a gain above 100 is
            // treated as a bug so the UI has a red tick to
            // show. (Exception values are not surfaced yet.)
            if (v > 100.0)
                throw std::runtime_error { "gain out of range" };

            return v;
        })
    };

    // worker: a count of triggered events. The scan lives on the ui clock
    // and the result crosses latest, so the worker never sees a monostate
    // (which is what a queued crossing of an unset input would stage).
    const auto trigger {
        shell::share(shell::input<std::int64_t, clock_tag::ui>("trigger"))
    };
    const auto triggers { shell::at<clock_tag::worker, conc::policy::latest>(
        trigger >>
        shell::scan<std::int64_t, std::int64_t>(
            0, [](const std::int64_t& previous, const std::int64_t&) {
                return previous + 1;
            }
        )
    ) };

    // audio: the length of the ui's loop region. A user struct crossing a
    // clock on a latest link — the UI renders it inline in the graph.
    const auto loop { shell::input<loop_region, clock_tag::ui>("loop") };
    const auto loop_len {
        shell::at<clock_tag::audio, conc::policy::latest>(loop) >>
        shell::lift<loop_region, double>([](const loop_region& region) {
            return region.end - region.start;
        }) >>
        shell::output<double, clock_tag::audio>("loop_len")
    };

    return shell::parallel(
        tempo >> shell::output<double, clock_tag::audio>("tempo"),
        ticks >> shell::output<std::int64_t, clock_tag::audio>("ticks"),
        gain_out >> shell::output<double, clock_tag::ui>("gain_out"),
        triggers >> shell::output<std::int64_t, clock_tag::worker>("triggers"),
        loop_len
    );
}
} // namespace

int
main(int argc, char** argv)
{
    using namespace clockr;

    std::string host { "127.0.0.1" };
    std::uint16_t port { 9002 };
    std::string web_root { "tools/web-debugger/web" };
    std::int64_t throw_every { 0 }; // 0 = never

    for (int i { 1 }; i < argc; ++i)
    {
        const std::string_view arg { argv[i] };

        if (arg == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--web-root" && i + 1 < argc)
            web_root = argv[++i];
        else if (arg == "--throw-every" && i + 1 < argc)
            throw_every = std::atoll(argv[++i]);
    }

    const auto demo_network_value { demo_network() };

    kernel::program demo_program { demo_network_value };

    debug::trace_recorder demo_recorder;

    debug::server server {
        {
            { "demo", demo_program, demo_recorder },
        },
        { .host = host, .port = port, .web_root = web_root },
    };

    // The command queues and the debugger's tick-boundary hook, one pair per
    // example: the hook is attached to every engine that ticks its example, so
    // the tick loops below stay exactly `engine.step(program, clock)` — the
    // same call for a debug_engine or a release_engine. Declared before the
    // engines so each hook outlives its attachments.
    debug::example_commands& demo_commands { *server.commands_for("demo") };
    debug::command_hook demo_hook { demo_commands };

    // One debug_engine per ticking thread per example (the sequence counter
    // and tick context are plain members); each pair shares its example's
    // recorder queues, which is what quarantines the timelines. The same hook
    // instance serves all of an example's regions: the engine runs it on the
    // region's own ticking thread, and the per-region queues keep the SPSC
    // discipline.
    kernel::debug_engine demo_audio_engine { demo_recorder };
    demo_audio_engine.set_step_hook(&demo_hook);
    kernel::debug_engine demo_ui_engine { demo_recorder };
    demo_ui_engine.set_step_hook(&demo_hook);
    kernel::debug_engine demo_worker_engine { demo_recorder };
    demo_worker_engine.set_step_hook(&demo_hook);

    std::atomic<bool> running { true };

    // Startup discipline (as in the region-tick tests): publish once on the
    // producer clock before the consumer clocks run, so the worms have a real
    // value to hold instead of staging monostate through a partial lift.
    demo_program.set("bpm", 60.0);
    demo_program.set("gain", 0.0);
    demo_program.set("trigger", 0);
    demo_program.set<loop_region>("loop", 0.0, 4.0, 1);
    demo_ui_engine.step(demo_program, kernel::clock_tag::ui);

    // The ui clock drives the example's inputs, then the other clocks run
    // their own cadence. Cadences are simulated (sleep-based), not real audio.
    std::thread ui_thread { [&] {
        std::int64_t tick { 0 };

        while (running.load(std::memory_order_relaxed))
        {
            try
            {
                demo_program.set("bpm", 60.0 + static_cast<double>(tick % 60));
                demo_program.set("gain", static_cast<double>(tick % 50));
                demo_program.set<loop_region>(
                    "loop", 0.0, static_cast<double>((tick % 8) + 4), tick % 4
                );

                if (throw_every > 0 &&
                    (tick % throw_every) == (throw_every - 1))
                    demo_program.set("gain", 999.0); // trips the throwing lift

                if ((tick % 4) == 0)
                    demo_program.set("trigger", tick);

                demo_ui_engine.step(demo_program, kernel::clock_tag::ui);
                ++tick;
            }
            catch (const std::exception& error)
            {
                // The demo survives its own throwing node: the throw is
                // the point, and the trace already recorded it.
                std::cerr << "[debugger_demo] ui tick threw: " << error.what()
                          << "\n";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds { 33 });
        }
    } };

    std::thread audio_thread { [&] {
        while (running.load(std::memory_order_relaxed))
        {
            try
            {
                demo_audio_engine.step(demo_program, kernel::clock_tag::audio);
            }
            catch (const std::exception& error)
            {
                std::cerr << "[debugger_demo] audio tick threw: "
                          << error.what() << "\n";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds { 3 });
        }
    } };

    std::thread worker_thread { [&] {
        while (running.load(std::memory_order_relaxed))
        {
            try
            {
                demo_worker_engine.step(
                    demo_program, kernel::clock_tag::worker
                );
            }
            catch (const std::exception& error)
            {
                std::cerr << "[debugger_demo] worker tick threw: "
                          << error.what() << "\n";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
        }
    } };

    std::cout << "[debugger_demo] serving the debug UI on http://" << host
              << ":" << port << " — open it in a browser; Ctrl-C to stop\n";

    server.run(); // blocks on the uWS event loop

    running.store(false, std::memory_order_relaxed);
    ui_thread.join();
    audio_thread.join();
    worker_thread.join();

    return 0;
}
