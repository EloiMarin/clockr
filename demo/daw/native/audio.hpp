#pragma once

// audio.hpp — the native demo's audio device backend.
//
// Thin wrapper over miniaudio's device API: a real playback device when one
// can be opened, the null backend otherwise. Both drive the same callback on a
// real OS thread, so the audio region always has a genuine callback site to
// tick from — the only difference is whether the samples reach a speaker.
//
// The callback signature is deliberately plain (no std::function): it runs on
// the device's real-time thread and must not allocate or throw.

#include <cstdint>
#include <memory>
#include <string>

namespace daw::native
{

struct audio_device_config
{
    std::uint32_t period_frames { 256 };
    double sample_rate { 48000.0 };
    std::uint32_t channels { 2 };
};

using audio_callback_fn = void (*)(
    void* user,
    float* output,
    std::uint32_t frames,
    double sample_rate,
    std::uint32_t channels
);

class audio_device
{
public:
    audio_device();
    ~audio_device();

    audio_device(const audio_device&) = delete;
    audio_device&
    operator=(const audio_device&) = delete;
    audio_device(audio_device&&) = delete;
    audio_device&
    operator=(audio_device&&) = delete;

    /// Open and start a playback device. Tries the default backend first, then
    /// miniaudio's null backend; with `force_null` only the latter is tried.
    /// On failure returns false and fills `error`; on success `error` is empty.
    bool
    start(
        const audio_device_config& config,
        bool force_null,
        audio_callback_fn callback,
        void* user,
        std::string& error
    );

    /// Stop and close the device. Blocks until the callback is no longer
    /// running. Safe to call when not running.
    void
    stop();

    /// Stop, re-open with `config`, and start again. On failure the previous
    /// configuration is restored where possible; returns false and fills
    /// `error`.
    bool
    reconfigure(const audio_device_config& config, std::string& error);

    /// The currently configured device parameters (not necessarily the last
    /// callback's block size; the callback receives the exact frame count).
    audio_device_config
    config() const;

    /// True when the null backend is in use (no audible output).
    bool
    is_null() const;

    bool
    running() const;

private:
    struct impl;
    std::unique_ptr<impl> m_impl;
};

} // namespace daw::native
