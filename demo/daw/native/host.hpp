#pragma once

// host.hpp — the native DAWmulator's engine host.
//
// It owns the program and the two region threads:
//
//   ui thread     applies ui commands, steps the ui region at 60 Hz (or on a
//                 manual step), publishes a ui snapshot;
//   audio thread  miniaudio's device callback: sets the block descriptor,
//                 steps the audio region, publishes an audio snapshot, and
//                 drives the dumb synth.
//
// The network thread never touches the program: it enqueues commands and reads
// snapshots. That is the same tick-boundary discipline the library documents
// for `program::set`/`get` (same-thread-with-the-owning-region), just with the
// browser on the other side of a socket.
//
// `restart()` stops both threads, rebuilds the program from the recipe, and
// starts them again; it is safe only while no tick is in flight (the stop is
// part of the operation).

#include "audio.hpp"
#include "synth.hpp"
#include "types.hpp"

#include "clockr/conc/slot.hpp"
#include "clockr/conc/spsc.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace daw::native
{

namespace conc = clockr::conc;
namespace kernel = clockr::kernel;
namespace shell = clockr::shell;

/// A fixed-size, allocation-free SPSC ring for per-block audio snapshots.
/// The network thread drains it at its own cadence, so no note-on or meter
/// sample is coalesced away by a slower UI tick.
class live_debug;

class audio_event_queue
{
public:
    bool
    push(const audio_snapshot& snapshot) noexcept;

    bool
    pop(audio_snapshot& snapshot) noexcept;

    void
    reset() noexcept;

private:
    static constexpr std::size_t kCapacity { 1024 };

    std::array<audio_snapshot, kCapacity> m_items {};
    std::atomic<std::size_t> m_head { 0 }; // consumer
    std::atomic<std::size_t> m_tail { 0 }; // producer
};

struct host_options
{
    /// The audio region's pool budget. A real device ticks far more often than
    /// the tests, and `set<BlockDesc>` allocates a cell per block from the
    /// audio region's pool.
    std::size_t region_pool_bytes { 1U << 20 };
    std::size_t reclaim_batch { 256 };

    bool force_null_audio { false };
    std::uint32_t audio_frames { 256 };
    double sample_rate { 48000.0 };

    /// The wasm page's auto loops start on. The selftest turns them off and
    /// drives manual steps for determinism.
    bool auto_ui { true };
    bool auto_audio { true };

    /// The live MCP bridge (debug::server + trace recorder + command hook).
    /// Present only when the build linked the debugger library. `--selftest`
    /// and `--no-debug` leave it off.
    bool enable_debugger { true };
    std::string debug_host { "127.0.0.1" };
    std::uint16_t debug_port { 9002 };
    std::string debug_web_root {};
};

class host
{
public:
    explicit host(host_options options = {});
    ~host();

    host(const host&) = delete;
    host&
    operator=(const host&) = delete;
    host(host&&) = delete;
    host&
    operator=(host&&) = delete;

    bool
    start(std::string& error);

    void
    stop();

    bool
    restart(std::string& error);

    /// Stop and re-open the audio device with a new block size/sample rate.
    /// Runs on the caller's (non-audio) thread; the device stop waits for the
    /// callback to finish, so the engine is never re-entered.
    bool
    reconfigure_audio(
        std::uint32_t n_frames,
        double sample_rate,
        std::string& error
    );

    bool
    send_ui(const ui_command& command);

    bool
    send_audio(const audio_command& command);

    void
    set_auto_ui(bool on) noexcept;

    void
    set_auto_audio(bool on) noexcept;

    ui_snapshot
    ui() const;

    audio_snapshot
    audio() const;

    bool
    pop_audio_event(audio_snapshot& snapshot) noexcept;

    controls_snapshot
    controls() const;

    audio_config_snapshot
    audio_config() const;

    bool
    null_audio() const;

    bool
    auto_ui() const noexcept;

    bool
    auto_audio() const noexcept;

    bool
    running() const;

    /// Read and clear the last error, so the network thread reports each one
    /// once instead of once per timer tick.
    host_error
    take_error() noexcept;

private:
    bool
    start_locked(std::string& error);

    void
    stop_locked();

    void
    ui_loop();

    void
    on_audio(
        float* output,
        std::uint32_t frames,
        double sample_rate,
        std::uint32_t channels
    );

    void
    apply_ui(const ui_command& command);

    void
    publish_ui();

    void
    publish_controls();

    void
    refresh_controls();

    audio_outputs
    read_audio_outputs() const;

    static void
    audio_callback(
        void* user,
        float* output,
        std::uint32_t frames,
        double sample_rate,
        std::uint32_t channels
    );

    host_options m_options;
    std::unique_ptr<kernel::program> m_program;
#ifdef DAW_NATIVE_WITH_DEBUGGER
    std::unique_ptr<live_debug> m_live_debug;
#endif
    kernel::release_engine m_release_ui_engine;
    kernel::release_engine m_release_audio_engine;
    kernel::engine* m_ui_engine { nullptr };
    kernel::engine* m_audio_engine { nullptr };
    audio_device m_device;
    synth_state m_synth;

    std::unique_ptr<conc::spsc<ui_command>> m_ui_commands;
    std::unique_ptr<conc::spsc<audio_command>> m_audio_commands;

    conc::slot<ui_snapshot> m_ui_snapshot;
    conc::slot<audio_snapshot> m_audio_snapshot;
    audio_event_queue m_audio_events;
    conc::slot<controls_snapshot> m_controls_snapshot;

    std::atomic<bool> m_auto_ui;
    std::atomic<bool> m_auto_audio;
    std::atomic<bool> m_running;
    std::atomic<bool> m_stop;
    std::atomic<host_error> m_error;

    std::mutex m_lifecycle;
    std::mutex m_ui_wait_mutex;
    std::condition_variable m_ui_wait;
    std::thread m_ui_thread;

    // Only the ui thread (or the quiescent start/stop path) touches these.
    controls_snapshot m_controls;
    std::uint64_t m_ui_seq { 0 };

    // Only the audio callback (or the quiescent start/stop path) touches these.
    std::uint64_t m_audio_block { 0 };
    std::int64_t m_start_sample { 0 };
};

} // namespace daw::native
