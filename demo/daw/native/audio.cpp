#include "audio.hpp"

#include <miniaudio.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace daw::native
{

namespace
{
std::string
describe(ma_result result)
{
    return std::string { ma_result_description(result) };
}
} // namespace

struct audio_device::impl
{
    ma_device device {};
    audio_device_config config {};
    audio_callback_fn callback { nullptr };
    void* user { nullptr };
    bool initialized { false };
    bool force_null { false };
    mutable std::mutex mutex;
    std::atomic<bool> null_backend { false };
    std::atomic<bool> running { false };

    static void
    data_callback(
        ma_device* device,
        void* output,
        const void* input,
        ma_uint32 frame_count
    )
    {
        static_cast<void>(input);

        auto* self { static_cast<impl*>(device->pUserData) };

        if (self == nullptr || output == nullptr)
            return;

        const auto frames { static_cast<std::uint32_t>(frame_count) };

        if (self->callback != nullptr)
        {
            self->callback(
                self->user,
                static_cast<float*>(output),
                frames,
                self->config.sample_rate,
                self->config.channels
            );
            return;
        }

        std::memset(
            output,
            0,
            static_cast<std::size_t>(frames) * self->config.channels *
                sizeof(float)
        );
    }

    bool
    open(const audio_device_config& next, bool only_null, std::string& error)
    {
        std::memset(&device, 0, sizeof(device));

        ma_device_config device_config {
            ma_device_config_init(ma_device_type_playback)
        };

        device_config.playback.format = ma_format_f32;
        device_config.playback.channels = next.channels;
        device_config.sampleRate = static_cast<ma_uint32>(next.sample_rate);
        device_config.periodSizeInFrames = next.period_frames;
        device_config.dataCallback = &impl::data_callback;
        device_config.pUserData = this;

        auto result { MA_ERROR };
        bool selected_null { only_null };

        if (!only_null)
        {
            result =
                ma_device_init_ex(nullptr, 0, nullptr, &device_config, &device);

            if (result == MA_SUCCESS)
            {
                selected_null = device.pContext != nullptr &&
                                device.pContext->backend == ma_backend_null;
            }
        }

        if (only_null || result != MA_SUCCESS)
        {
            // No usable default device: fall back to miniaudio's null backend,
            // which still calls back from a real thread. The failed attempt
            // released its context; reset the device before re-initializing.
            std::memset(&device, 0, sizeof(device));

            const ma_backend only_null_backend { ma_backend_null };

            result = ma_device_init_ex(
                &only_null_backend, 1, nullptr, &device_config, &device
            );
            selected_null = true;
        }

        if (result != MA_SUCCESS)
        {
            error = "audio device init failed: " + describe(result);
            return false;
        }

        initialized = true;
        config = next;
        force_null = only_null;
        null_backend.store(selected_null, std::memory_order_relaxed);

        result = ma_device_start(&device);

        if (result != MA_SUCCESS)
        {
            error = "audio device start failed: " + describe(result);
            ma_device_uninit(&device);
            initialized = false;
            return false;
        }

        running.store(true, std::memory_order_release);
        error.clear();
        return true;
    }

    void
    close()
    {
        if (!initialized)
            return;

        running.store(false, std::memory_order_release);
        ma_device_uninit(&device);
        initialized = false;
        null_backend.store(false, std::memory_order_relaxed);
    }
};

audio_device::audio_device() :
    m_impl { std::make_unique<impl>() }
{}

audio_device::~audio_device()
{
    stop();
}

bool
audio_device::start(
    const audio_device_config& config,
    bool force_null,
    audio_callback_fn callback,
    void* user,
    std::string& error
)
{
    std::lock_guard<std::mutex> lock { m_impl->mutex };

    m_impl->close();

    m_impl->callback = callback;
    m_impl->user = user;

    return m_impl->open(config, force_null, error);
}

void
audio_device::stop()
{
    std::lock_guard<std::mutex> lock { m_impl->mutex };

    m_impl->close();
}

bool
audio_device::reconfigure(const audio_device_config& config, std::string& error)
{
    std::lock_guard<std::mutex> lock { m_impl->mutex };

    const audio_device_config previous { m_impl->config };

    m_impl->close();

    if (m_impl->open(config, m_impl->force_null, error))
        return true;

    // Restore the working configuration so a rejected gesture does not leave
    // the demo silent until restart.
    std::string restore_error;
    m_impl->open(previous, m_impl->force_null, restore_error);

    return false;
}

audio_device_config
audio_device::config() const
{
    std::lock_guard<std::mutex> lock { m_impl->mutex };

    return m_impl->config;
}

bool
audio_device::is_null() const
{
    return m_impl->null_backend.load(std::memory_order_relaxed);
}

bool
audio_device::running() const
{
    return m_impl->running.load(std::memory_order_acquire);
}

} // namespace daw::native
