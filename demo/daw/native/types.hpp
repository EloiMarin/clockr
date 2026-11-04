#pragma once

// types.hpp — the plain-data language the native host, its threads, the
// player and the browser bridge exchange.
//
// Everything here is trivially copyable: snapshots travel through
// `conc::slot` (a seqlock) from the ticking threads to the network thread, and
// commands travel through `conc::spsc` from the network thread to the owning
// region's thread. No std::string, no pointers, no allocation on the audio
// callback.

#include <cstdint>

namespace daw::native
{

struct audio_outputs
{
    double playhead { 0.0 };
    std::int64_t midi_count { 0 };
    std::int64_t midi_offset { -1 };
    std::int64_t midi_off_count { 0 };
    double voice_gain { 0.0 };
};

struct audio_snapshot
{
    std::uint64_t block { 0 };
    std::int64_t start_sample { 0 };
    std::uint32_t n_frames { 0 };
    double sample_rate { 0.0 };
    bool has_outputs { false };
    audio_outputs outputs {};
};

struct ui_snapshot
{
    std::uint64_t seq { 0 };
    double playhead_ui { 0.0 };
    bool has_playhead_ui { false };
};

struct controls_snapshot
{
    bool playing { false };
    double bpm { 120.0 };
    double master { 0.8 };
    double loop_start { 0.0 };
    double loop_end { 4.0 };
    bool loop_on { true };
};

struct audio_config_snapshot
{
    std::uint32_t n_frames { 0 };
    double sample_rate { 0.0 };
};

enum class ui_command_kind : std::uint8_t
{
    set_playing,
    set_bpm,
    set_master,
    set_loop,
    set_seek,
    clear_seek,
    step,
};

struct ui_command
{
    ui_command_kind kind { ui_command_kind::step };
    bool on { false };
    double a { 0.0 };
    double b { 0.0 };
    double c { 0.0 };
};

enum class audio_command_kind : std::uint8_t
{
    step,
};

struct audio_command
{
    audio_command_kind kind { audio_command_kind::step };
};

enum class host_error : std::uint8_t
{
    none,
    audio_tick_threw,
    ui_tick_threw,
    audio_device_failed,
};

} // namespace daw::native
