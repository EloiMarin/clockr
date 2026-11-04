// dawmulator_native — the DAWmulator as a live native process.
//
// The engine runs on real OS threads (a ui thread and miniaudio's audio
// callback thread) and the existing demo page is served over a WebSocket.
// The page is the same one the wasm build stages; app.js only switches its
// transport when it sees `?native=1`.
//
//   ./dawmulator_native                 # serve on http://127.0.0.1:9003
//   ./dawmulator_native --open          # ... and open the browser
//   ./dawmulator_native --null-audio    # silent, no device
//   ./dawmulator_native --selftest      # headless, deterministic (CI)

#include "host.hpp"
#include "server.hpp"

#include "types.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifdef DAW_NATIVE_WEB_ROOT
constexpr std::string_view kDefaultWebRoot { DAW_NATIVE_WEB_ROOT };
#else
constexpr std::string_view kDefaultWebRoot { "demo/daw/shell" };
#endif

namespace
{

using namespace daw::native;

bool
wait_until(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout
)
{
    const auto deadline { std::chrono::steady_clock::now() + timeout };

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    }

    return predicate();
}

bool
near(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-9;
}

bool
check(bool condition, std::string_view what, std::string_view detail = {})
{
    if (condition)
    {
        std::cout << "ok   " << what << "\n";
        return true;
    }

    std::cout << "FAIL " << what;

    if (!detail.empty())
        std::cout << " (" << detail << ")";

    std::cout << "\n";
    return false;
}

struct audio_check
{
    double playhead;
    std::int64_t midi_count;
    std::int64_t midi_offset;
    std::int64_t midi_off_count;
    double voice_gain;
};

int
run_selftest()
{
    host_options options;
    options.auto_ui = false;
    options.auto_audio = false;
    options.force_null_audio = true;
    options.audio_frames = 24000; // one beat at 48000/120 bpm
    options.sample_rate = 48000.0;
    options.enable_debugger = false;

    host engine { options };

    std::string error;

    if (!engine.start(error))
    {
        std::cerr << "FAIL host start: " << error << "\n";
        return 1;
    }

    bool ok { true };

    auto send_ui = [&](ui_command command) {
        if (!engine.send_ui(command))
        {
            std::cerr << "FAIL ui command queue full\n";
            ok = false;
        }
    };

    auto step_ui = [&] {
        const auto before { engine.ui().seq };
        ui_command command;
        command.kind = ui_command_kind::step;
        send_ui(command);

        if (!wait_until(
                [&] {
                    return engine.ui().seq > before;
                },
                std::chrono::milliseconds { 2000 }
            ))
        {
            std::cerr << "FAIL ui step timed out\n";
            ok = false;
        }
    };

    auto step_audio = [&](std::uint64_t target_block) {
        audio_command command;
        command.kind = audio_command_kind::step;

        if (!engine.send_audio(command))
        {
            std::cerr << "FAIL audio command queue full\n";
            ok = false;
            return audio_snapshot {};
        }

        if (!wait_until(
                [&] {
                    return engine.audio().block >= target_block;
                },
                std::chrono::milliseconds { 4000 }
            ))
        {
            std::cerr << "FAIL audio step timed out at block " << target_block
                      << "\n";
            ok = false;
        }

        return engine.audio();
    };

    // §6.1, exactly as tests/demo.test.cpp drives it: playing, 120 bpm, loop
    // (0,4] on, five 1-beat blocks. The note at 3.5 is one beat long, so its
    // off crossing wraps to 0.5.
    ui_command playing;
    playing.kind = ui_command_kind::set_playing;
    playing.on = true;
    send_ui(playing);

    ui_command bpm;
    bpm.kind = ui_command_kind::set_bpm;
    bpm.a = 120.0;
    send_ui(bpm);

    ui_command loop;
    loop.kind = ui_command_kind::set_loop;
    loop.a = 0.0;
    loop.b = 4.0;
    loop.on = true;
    send_ui(loop);

    ui_command master;
    master.kind = ui_command_kind::set_master;
    master.a = 0.8;
    send_ui(master);

    step_ui();

    const audio_check expected[5] {
        { 1.0, 0, -1, 1, 0.0 }, { 2.0, 0, -1, 0, 0.0 },
        { 3.0, 0, -1, 0, 0.0 }, { 0.0, 1, 12000, 0, 0.8 },
        { 1.0, 0, -1, 1, 0.0 },
    };

    for (std::uint64_t k { 0 }; k < 5; ++k)
    {
        const auto snapshot { step_audio(k + 1) };

        ok = check(snapshot.has_outputs, "audio outputs published") && ok;
        ok = check(
                 snapshot.start_sample == static_cast<std::int64_t>(k) * 24000,
                 "block start sample",
                 std::to_string(snapshot.start_sample)
             ) &&
             ok;
        ok = check(
                 near(snapshot.outputs.playhead, expected[k].playhead),
                 "playhead"
             ) &&
             ok;
        ok = check(
                 snapshot.outputs.midi_count == expected[k].midi_count,
                 "midi_count"
             ) &&
             ok;
        ok = check(
                 snapshot.outputs.midi_offset == expected[k].midi_offset,
                 "midi_offset"
             ) &&
             ok;
        ok = check(
                 snapshot.outputs.midi_off_count == expected[k].midi_off_count,
                 "midi_off_count"
             ) &&
             ok;
        ok = check(
                 near(snapshot.outputs.voice_gain, expected[k].voice_gain),
                 "voice_gain"
             ) &&
             ok;
    }

    // The playhead crossed back to ui: the newest audio sample at ui cadence.
    const auto audio_after_blocks { engine.audio() };
    step_ui();
    ok = check(engine.ui().has_playhead_ui, "playhead_ui produced") && ok;
    ok = check(
             near(engine.ui().playhead_ui, audio_after_blocks.outputs.playhead),
             "playhead_ui samples the latest audio"
         ) &&
         ok;

    // Pause freezes the transport; seek-while-paused jumps absolutely.
    ui_command pause;
    pause.kind = ui_command_kind::set_playing;
    pause.on = false;
    send_ui(pause);
    step_ui();

    const auto paused_block { step_audio(6) };
    ok = check(
             near(paused_block.outputs.playhead, 1.0),
             "pause freezes the playhead"
         ) &&
         ok;

    ui_command seek;
    seek.kind = ui_command_kind::set_seek;
    seek.a = 2.5;
    send_ui(seek);
    step_ui();

    const auto sought_block { step_audio(7) };
    ok = check(
             near(sought_block.outputs.playhead, 2.5), "seek lands while paused"
         ) &&
         ok;

    step_ui();
    ok = check(near(engine.ui().playhead_ui, 2.5), "cursor follows the seek") &&
         ok;

    engine.stop();

    if (ok)
        std::cout << "daw.native.selftest: ok\n";
    else
        std::cout << "daw.native.selftest: FAILED\n";

    return ok ? 0 : 1;
}

void
print_usage()
{
    std::cout
        << "dawmulator_native — the DAWmulator as a native process\n"
           "\n"
           "  --host ADDR         interface to bind (default 127.0.0.1)\n"
           "  --port N            port to bind (default 9003)\n"
           "  --web-root DIR      directory holding "
           "index.html/app.js/style.css\n"
           "  --null-audio        force the silent null audio backend\n"
           "  --open              open the demo page in a browser\n"
           "  --block N           audio period in frames (default 256)\n"
           "  --rate HZ           audio sample rate (default 48000)\n"
           "  --pool-bytes N      per-region value pool size (default "
           "1048576)\n"
           "  --no-debug          do not start the MCP debugger server\n"
           "  --debug-host ADDR   MCP debugger bind address (default "
           "127.0.0.1)\n"
           "  --debug-port N      MCP debugger port (default 9002)\n"
           "  --debug-web-root DIR  directory holding the debugger web UI\n"
           "  --selftest          run the headless deterministic smoke test\n"
           "  --help              this text\n";
}

} // namespace

int
main(int argc, char** argv)
{
    host_options host_options_value;
    server_options server_options_value;

    server_options_value.web_root = std::string { kDefaultWebRoot };

    bool selftest { false };

    for (int i { 1 }; i < argc; ++i)
    {
        const std::string_view arg { argv[i] };

        if (arg == "--selftest")
            selftest = true;
        else if (arg == "--help" || arg == "-h")
        {
            print_usage();
            return 0;
        }
        else if (arg == "--open")
            server_options_value.open_browser = true;
        else if (arg == "--null-audio")
            host_options_value.force_null_audio = true;
        else if (arg == "--no-debug")
            host_options_value.enable_debugger = false;
        else if (arg == "--debug-host" && i + 1 < argc)
            host_options_value.debug_host = argv[++i];
        else if (arg == "--debug-port" && i + 1 < argc)
            host_options_value.debug_port =
                static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--debug-web-root" && i + 1 < argc)
            host_options_value.debug_web_root = argv[++i];
        else if (arg == "--host" && i + 1 < argc)
            server_options_value.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)
            server_options_value.port =
                static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--web-root" && i + 1 < argc)
            server_options_value.web_root = argv[++i];
        else if (arg == "--block" && i + 1 < argc)
            host_options_value.audio_frames =
                static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (arg == "--rate" && i + 1 < argc)
            host_options_value.sample_rate = std::atof(argv[++i]);
        else if (arg == "--pool-bytes" && i + 1 < argc)
            host_options_value.region_pool_bytes =
                static_cast<std::size_t>(std::atoll(argv[++i]));
        else
        {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage();
            return 1;
        }
    }

    if (selftest)
        return run_selftest();

    host engine { host_options_value };

    std::string error;

    if (!engine.start(error))
    {
        std::cerr << "[dawmulator] engine start failed: " << error << "\n";
        return 1;
    }

#ifdef DAW_NATIVE_WITH_DEBUGGER
    if (host_options_value.enable_debugger)
    {
        std::cout << "[dawmulator] MCP debugger on ws://"
                  << host_options_value.debug_host << ":"
                  << host_options_value.debug_port << "/ws (example dawmulator)"
                  << std::endl;
    }
#endif

    server bridge { engine, std::move(server_options_value) };

    const bool served { bridge.run() };

    engine.stop();

    return served ? 0 : 1;
}
