#include "host.hpp"

#ifdef DAW_NATIVE_WITH_DEBUGGER
#include "debug.hpp"
#endif

#include "daw/dawmulator.hpp"
#include "daw/transport.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/shell/recipe.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace daw::native
{

namespace
{
constexpr auto kUiPeriod { std::chrono::milliseconds { 16 } };
constexpr std::size_t kCommandCapacity { 1024 };
constexpr double kNoValue { std::numeric_limits<double>::quiet_NaN() };
} // namespace

bool
audio_event_queue::push(const audio_snapshot& snapshot) noexcept
{
    const auto tail { m_tail.load(std::memory_order_relaxed) };
    const auto next { (tail + 1) % kCapacity };

    if (next == m_head.load(std::memory_order_acquire))
        return false;

    m_items[tail] = snapshot;
    m_tail.store(next, std::memory_order_release);
    return true;
}

bool
audio_event_queue::pop(audio_snapshot& snapshot) noexcept
{
    const auto head { m_head.load(std::memory_order_relaxed) };

    if (head == m_tail.load(std::memory_order_acquire))
        return false;

    snapshot = m_items[head];
    m_head.store((head + 1) % kCapacity, std::memory_order_release);
    return true;
}

void
audio_event_queue::reset() noexcept
{
    m_head.store(0, std::memory_order_relaxed);
    m_tail.store(0, std::memory_order_relaxed);
}

host::host(host_options options) :
    m_options { options },
    m_auto_ui { options.auto_ui },
    m_auto_audio { options.auto_audio },
    m_running { false },
    m_stop { false },
    m_error { host_error::none }
{}

host::~host()
{
    stop();
}

bool
host::start(std::string& error)
{
    std::lock_guard<std::mutex> lock { m_lifecycle };

    return start_locked(error);
}

void
host::stop()
{
    std::lock_guard<std::mutex> lock { m_lifecycle };

    stop_locked();
}

bool
host::restart(std::string& error)
{
    std::lock_guard<std::mutex> lock { m_lifecycle };

    stop_locked();

    return start_locked(error);
}

bool
host::reconfigure_audio(
    std::uint32_t n_frames,
    double sample_rate,
    std::string& error
)
{
    std::lock_guard<std::mutex> lock { m_lifecycle };

    if (!m_running.load(std::memory_order_acquire))
    {
        error = "engine is not running";
        return false;
    }

    if (n_frames == 0 || sample_rate <= 0.0)
    {
        error = "invalid block size or sample rate";
        return false;
    }

    const audio_device_config config { n_frames, sample_rate, 2 };

    return m_device.reconfigure(config, error);
}

bool
host::send_ui(const ui_command& command)
{
    if (m_ui_commands == nullptr)
        return false;

    return m_ui_commands->send(command);
}

bool
host::send_audio(const audio_command& command)
{
    if (m_audio_commands == nullptr)
        return false;

    return m_audio_commands->send(command);
}

void
host::set_auto_ui(bool on) noexcept
{
    m_auto_ui.store(on, std::memory_order_relaxed);
}

void
host::set_auto_audio(bool on) noexcept
{
    m_auto_audio.store(on, std::memory_order_relaxed);
}

ui_snapshot
host::ui() const
{
    return m_ui_snapshot.load();
}

audio_snapshot
host::audio() const
{
    return m_audio_snapshot.load();
}

bool
host::pop_audio_event(audio_snapshot& snapshot) noexcept
{
    return m_audio_events.pop(snapshot);
}

controls_snapshot
host::controls() const
{
    return m_controls_snapshot.load();
}

audio_config_snapshot
host::audio_config() const
{
    const auto config { m_device.config() };

    return { config.period_frames, config.sample_rate };
}

bool
host::null_audio() const
{
    return m_device.is_null();
}

bool
host::auto_ui() const noexcept
{
    return m_auto_ui.load(std::memory_order_relaxed);
}

bool
host::auto_audio() const noexcept
{
    return m_auto_audio.load(std::memory_order_relaxed);
}

bool
host::running() const
{
    return m_running.load(std::memory_order_acquire);
}

host_error
host::take_error() noexcept
{
    return m_error.exchange(host_error::none, std::memory_order_acq_rel);
}

bool
host::start_locked(std::string& error)
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    error.clear();

    kernel::program_options program_options;
    program_options.region_pool_bytes = m_options.region_pool_bytes;
    program_options.reclaim_batch = m_options.reclaim_batch;

    m_program = std::make_unique<kernel::program>(
        daw::dawmulator_recipe(), program_options
    );
    m_ui_commands = std::make_unique<conc::spsc<ui_command>>(kCommandCapacity);
    m_audio_commands =
        std::make_unique<conc::spsc<audio_command>>(kCommandCapacity);

    m_controls = controls_snapshot {};
    m_ui_seq = 0;
    m_audio_block = 0;
    m_start_sample = 0;
    reset_synth(m_synth);

    m_ui_snapshot.store(ui_snapshot {});
    m_audio_snapshot.store(audio_snapshot {});
    m_audio_events.reset();
    m_controls_snapshot.store(m_controls);
    m_error.store(host_error::none, std::memory_order_relaxed);

    m_ui_engine = nullptr;
    m_audio_engine = nullptr;

#ifdef DAW_NATIVE_WITH_DEBUGGER
    bool debug_started { false };

    if (m_options.enable_debugger)
    {
        try
        {
            live_debug_options debug_options;
            debug_options.host = m_options.debug_host;
            debug_options.port = m_options.debug_port;
            debug_options.web_root = m_options.debug_web_root;

            m_live_debug = std::make_unique<live_debug>(
                *m_program, std::move(debug_options)
            );
            m_ui_engine = &m_live_debug->ui_engine();
            m_audio_engine = &m_live_debug->audio_engine();
            debug_started = true;
        }
        catch (const std::exception& failure)
        {
            error = std::string { "failed to build the debug bridge: " } +
                    failure.what();
        }
    }

    if (m_live_debug == nullptr)
#endif
    {
        m_ui_engine = &m_release_ui_engine;
        m_audio_engine = &m_release_audio_engine;
    }

    if (error.empty() == false)
    {
        m_program.reset();
        m_ui_commands.reset();
        m_audio_commands.reset();
        return false;
    }

    // The shared construction: level the intent ports and tick ui once before
    // audio runs, so a latest link carries real levels rather than monostate.
    daw::init_dawmulator(*m_program, *m_ui_engine);

#ifdef DAW_NATIVE_WITH_DEBUGGER
    if (debug_started && !m_live_debug->start(error))
    {
        m_live_debug.reset();
        m_ui_engine = nullptr;
        m_audio_engine = nullptr;
        m_program.reset();
        m_ui_commands.reset();
        m_audio_commands.reset();
        return false;
    }
#endif

    m_stop.store(false, std::memory_order_release);

    const audio_device_config config {
        m_options.audio_frames,
        m_options.sample_rate,
        2,
    };

    if (!m_device.start(
            config,
            m_options.force_null_audio,
            &host::audio_callback,
            this,
            error
        ))
    {
        m_error.store(
            host_error::audio_device_failed, std::memory_order_relaxed
        );
#ifdef DAW_NATIVE_WITH_DEBUGGER
        m_live_debug.reset();
#endif
        m_ui_engine = nullptr;
        m_audio_engine = nullptr;
        m_program.reset();
        m_ui_commands.reset();
        m_audio_commands.reset();
        return false;
    }

    m_running.store(true, std::memory_order_release);

    try
    {
        m_ui_thread = std::thread { [this] {
            ui_loop();
        } };
    }
    catch (...)
    {
        m_running.store(false, std::memory_order_release);
        m_device.stop();
#ifdef DAW_NATIVE_WITH_DEBUGGER
        m_live_debug.reset();
#endif
        m_ui_engine = nullptr;
        m_audio_engine = nullptr;
        m_program.reset();
        m_ui_commands.reset();
        m_audio_commands.reset();
        error = "failed to start the ui thread";
        return false;
    }

    return true;
}

void
host::stop_locked()
{
    m_stop.store(true, std::memory_order_release);
    m_ui_wait.notify_all();

    // Stop the device first: its callback is the only audio tick, and
    // ma_device_stop waits for it to return before the program storage is
    // released.
    m_device.stop();

    if (m_ui_thread.joinable())
        m_ui_thread.join();

    m_running.store(false, std::memory_order_release);

    // The debug bridge holds the recorder, command queues and engines: destroy
    // it while the program (and its pools) is still alive.
#ifdef DAW_NATIVE_WITH_DEBUGGER
    m_live_debug.reset();
#endif
    m_ui_engine = nullptr;
    m_audio_engine = nullptr;

    m_program.reset();
    m_ui_commands.reset();
    m_audio_commands.reset();
}

void
host::ui_loop()
{
    using clock = std::chrono::steady_clock;

    auto next { clock::now() };

    while (!m_stop.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lock { m_ui_wait_mutex };

            m_ui_wait.wait_until(lock, next, [this] {
                return m_stop.load(std::memory_order_acquire);
            });
        }

        if (m_stop.load(std::memory_order_acquire))
            break;

        bool tick { m_auto_ui.load(std::memory_order_relaxed) };

        if (m_ui_commands != nullptr)
        {
            ui_command command;

            while (m_ui_commands->receive(command))
            {
                try
                {
                    apply_ui(command);
                }
                catch (...)
                {
                    m_error.store(
                        host_error::ui_tick_threw, std::memory_order_relaxed
                    );
                }

                if (command.kind == ui_command_kind::step)
                    tick = true;
            }
        }

        if (tick)
        {
            try
            {
                m_ui_engine->step(*m_program, kernel::clock_tag::ui);
                ++m_ui_seq;
                refresh_controls();
                publish_ui();
            }
            catch (...)
            {
                m_error.store(
                    host_error::ui_tick_threw, std::memory_order_relaxed
                );
            }
        }

        next += kUiPeriod;

        const auto now { clock::now() };

        if (next < now)
            next = now;
    }
}

void
host::on_audio(
    float* output,
    std::uint32_t frames,
    double sample_rate,
    std::uint32_t channels
)
{
    const auto sample_count { static_cast<std::size_t>(frames) * channels };

    std::fill_n(output, sample_count, 0.0F);

    std::uint32_t pending { 0 };

    if (m_audio_commands != nullptr)
    {
        audio_command command;

        while (m_audio_commands->receive(command))
        {
            if (command.kind == audio_command_kind::step)
                ++pending;
        }
    }

    if (!m_auto_audio.load(std::memory_order_relaxed) && pending == 0)
    {
        // The device keeps calling, but the transport is frozen: silence,
        // without disturbing the synth state so a resume continues the note.
        return;
    }

    if (pending > 0)
        --pending;

    const auto start_sample { m_start_sample };

    try
    {
        m_program->set<daw::BlockDesc>(
            "block",
            start_sample,
            static_cast<std::int64_t>(frames),
            sample_rate
        );

        m_audio_engine->step(*m_program, kernel::clock_tag::audio);

        m_start_sample += static_cast<std::int64_t>(frames);
        ++m_audio_block;

        const auto outputs { read_audio_outputs() };

        const audio_snapshot snapshot { m_audio_block, start_sample, frames,
                                        sample_rate,   true,         outputs };

        m_audio_snapshot.store(snapshot);
        static_cast<void>(m_audio_events.push(snapshot));

        render_synth(m_synth, outputs, output, frames, sample_rate, channels);
    }
    catch (...)
    {
        // Nothing may escape the device callback. The tick published nothing;
        // report it at the next network drain and keep the block timeline
        // moving so the display does not stall on an error.
        m_error.store(host_error::audio_tick_threw, std::memory_order_relaxed);
        m_start_sample += static_cast<std::int64_t>(frames);
        ++m_audio_block;
        reset_synth(m_synth);
    }
}

void
host::apply_ui(const ui_command& command)
{
    switch (command.kind)
    {
        case ui_command_kind::set_playing:
            m_controls.playing = command.on;
            m_program->set("playing", command.on);
            publish_controls();
            break;

        case ui_command_kind::set_bpm:
            m_controls.bpm = command.a;
            m_program->set("bpm", command.a);
            publish_controls();
            break;

        case ui_command_kind::set_master:
            m_controls.master = command.a;
            m_program->set("master", command.a);
            publish_controls();
            break;

        case ui_command_kind::set_loop:
            m_program->set<daw::LoopRegion>(
                "loop", command.a, command.b, command.on
            );
            m_controls.loop_start = command.a;
            m_controls.loop_end = command.b;
            m_controls.loop_on = command.on;
            publish_controls();
            break;

        case ui_command_kind::set_seek:
            m_program->set(
                "seek", shell::to_value(std::optional<double> { command.a })
            );
            break;

        case ui_command_kind::clear_seek:
            m_program->set("seek", kernel::value {});
            break;

        case ui_command_kind::step:
            break;

        default:
            break;
    }
}

void
host::refresh_controls()
{
    // The program is the truth after a boundary: a page command was applied
    // before the step and a debugger (MCP) command inside before_step, so
    // reading the ports back here makes the page follow remote writes too.
    // `block` is device configuration (the server tracks it separately) and
    // auto is a host policy; `loop` is read back for completeness even though
    // the scalar MCP path cannot inject a struct today.
    try
    {
        bool changed { false };

        const auto playing { m_program->get_input("playing") };

        if (const auto* value { std::get_if<bool>(&playing) };
            value != nullptr && *value != m_controls.playing)
        {
            m_controls.playing = *value;
            changed = true;
        }

        const auto bpm { m_program->get_input("bpm") };

        if (const auto* value { std::get_if<double>(&bpm) };
            value != nullptr && *value != m_controls.bpm)
        {
            m_controls.bpm = *value;
            changed = true;
        }

        const auto master { m_program->get_input("master") };

        if (const auto* value { std::get_if<double>(&master) };
            value != nullptr && *value != m_controls.master)
        {
            m_controls.master = *value;
            changed = true;
        }

        const auto loop {
            kernel::get_input<daw::LoopRegion>(*m_program, "loop")
        };

        if (loop.start != m_controls.loop_start ||
            loop.end != m_controls.loop_end ||
            loop.enabled != m_controls.loop_on)
        {
            m_controls.loop_start = loop.start;
            m_controls.loop_end = loop.end;
            m_controls.loop_on = loop.enabled;
            changed = true;
        }

        if (changed)
            publish_controls();
    }
    catch (...)
    {
        // A missing port is a construction bug, not a reason to kill the ui
        // thread; leave the last controls snapshot in place.
    }
}

void
host::publish_ui()
{
    const double playhead_ui {
        m_program->get_or<double>("playhead_ui", kNoValue)
    };

    m_ui_snapshot.store(
        ui_snapshot { m_ui_seq,
                      std::isfinite(playhead_ui) ? playhead_ui : 0.0,
                      std::isfinite(playhead_ui) }
    );
}

void
host::publish_controls()
{
    m_controls_snapshot.store(m_controls);
}

audio_outputs
host::read_audio_outputs() const
{
    return audio_outputs {
        m_program->get_or<double>("playhead", kNoValue),
        m_program->get_or<std::int64_t>("midi_count", 0),
        m_program->get_or<std::int64_t>("midi_offset", -1),
        m_program->get_or<std::int64_t>("midi_off_count", 0),
        m_program->get_or<double>("voice_gain", 0.0),
    };
}

void
host::audio_callback(
    void* user,
    float* output,
    std::uint32_t frames,
    double sample_rate,
    std::uint32_t channels
)
{
    static_cast<host*>(user)->on_audio(output, frames, sample_rate, channels);
}

} // namespace daw::native
