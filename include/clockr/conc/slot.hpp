#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace clockr::conc
{

/// A seqlock-validated double buffer for a trivially-copyable payload of any
/// size: the writer fills the inactive buffer and bumps a version; a reader
/// retries until the version it saw before and after the read match. Use
/// `double_buffer` for payloads that fit in one atomic word.
template<typename T>
requires std::is_trivially_copyable_v<T>
class slot
{
public:
    /// Initialize the slot with `initial`.
    explicit slot(T initial = {}) noexcept
    {
        write_words(0, initial);
    }

    slot(slot&& other) noexcept :
        m_version { 0 }
    {
        T value {};
        other.read_words(other.published_buffer(), value);
        write_words(0, value);
    }

    auto
    operator=(slot&& other) noexcept -> slot&
    {
        if (this == &other)
            return *this;

        T value {};
        other.read_words(other.published_buffer(), value);
        write_words(0, value);
        m_version.store(0, std::memory_order_relaxed);
        return *this;
    }

    /// Publish `value`; readers either see it whole or retry.
    void
    store(const T& value) noexcept
    {
        const auto version { m_version.load(std::memory_order_relaxed) };

        write_words((version + 1u) & 1u, value);

        std::atomic_thread_fence(std::memory_order_seq_cst);
        m_version.store(version + 1u, std::memory_order_release);
    }

    /// Read the current value, retrying until a consistent snapshot is seen.
    T
    load() const noexcept
    {
        for (;;)
        {
            const auto before { m_version.load(std::memory_order_acquire) };

            T value {};
            read_words(before & 1u, value);

            std::atomic_thread_fence(std::memory_order_seq_cst);
            const auto after { m_version.load(std::memory_order_acquire) };

            if (before == after)
                return value;
        }
    }

    /// Load with a reference taken *inside* the read window, for payloads that
    /// carry a refcount. The sequence is: read, validate, `retain` (which
    /// returns false when the payload is already dead), validate again, and
    /// only then accept.
    ///
    ///  * The *first* validation before the retain is what keeps a torn read
    ///    from touching a garbage handle: while the version is stable the
    ///    writer's in-flight store targets the other buffer, so the bytes are
    ///    consistent.
    ///  * The retain must be a retain-if-live: a blind increment could revive
    ///    a payload the writer just released, and the reader's later drop
    ///    would release it twice.
    ///  * The *second* validation after the retain closes the displacement
    ///    race: if the writer stored and released in between, the version
    ///    moved, and we release and retry. The grace period keeps the released
    ///    payload's memory (and refcount) intact for exactly this.
    template<typename retain_fn, typename release_fn>
    T
    load_retained(retain_fn&& retain, release_fn&& release) const noexcept
    {
        for (;;)
        {
            const auto before { m_version.load(std::memory_order_acquire) };

            T value {};
            read_words(before & 1u, value);

            std::atomic_thread_fence(std::memory_order_seq_cst);

            if (m_version.load(std::memory_order_acquire) != before)
                continue; // torn: nothing was used, nothing to release

            if (!retain(value))
                continue; // payload already dead: the writer moved on

            std::atomic_thread_fence(std::memory_order_seq_cst);

            if (m_version.load(std::memory_order_acquire) == before)
                return value;

            release(value);
        }
    }

private:
    static constexpr std::size_t words_per_buffer
    {
        (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t)
    };

    std::size_t
    published_buffer() const noexcept
    {
        return m_version.load(std::memory_order_relaxed) & 1u;
    }

    void
    write_words(std::size_t buffer, const T& value) noexcept
    {
        const auto* bytes { reinterpret_cast<const unsigned char*>(&value) };

        for (std::size_t word { 0 }; word < words_per_buffer; ++word)
        {
            const auto offset { word * sizeof(std::uint64_t) };
            const auto remaining { sizeof(T) - offset };
            const auto chunk { std::min(remaining, sizeof(std::uint64_t)) };

            std::uint64_t payload {};
            std::memcpy(&payload, bytes + offset, chunk);

            std::atomic_ref<std::uint64_t> word_ref { m_buffers[buffer][word] };
            word_ref.store(payload, std::memory_order_relaxed);
        }
    }

    void
    read_words(std::size_t buffer, T& value) const noexcept
    {
        auto* bytes { reinterpret_cast<unsigned char*>(&value) };

        for (std::size_t word { 0 }; word < words_per_buffer; ++word)
        {
            const auto offset { word * sizeof(std::uint64_t) };
            const auto remaining { sizeof(T) - offset };
            const auto chunk { std::min(remaining, sizeof(std::uint64_t)) };

            const auto payload
            {
                std::atomic_ref<std::uint64_t> { m_buffers[buffer][word] }
                    .load(std::memory_order_relaxed)
            };
            std::memcpy(bytes + offset, &payload, chunk);
        }
    }

    std::atomic<std::uint64_t> m_version { 0 };
    mutable std::array<std::array<std::uint64_t, words_per_buffer>, 2>
        m_buffers {};
};

} // namespace clockr::conc
