#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace clockr::conc
{

/// Payloads small enough to be stored in a single `std::atomic<T>` word.
template<typename T>
concept inlineable = std::is_trivially_copyable_v<T>
    && sizeof(T) <= sizeof(std::uint64_t);

/// A single atomic slot for an inlineable payload: the reader sees either the
/// previous or the new value, never a torn one. For larger payloads use
/// `slot`, which validates with a seqlock.
template<typename T>
requires inlineable<T>
class double_buffer
{
public:
    /// Initialize the slot with `initial`.
    explicit double_buffer(T initial = {}) noexcept :
        m_value { initial }
    {}

    /// Publish `value` (release semantics).
    void
    store(const T& value) noexcept
    {
        m_value.store(value, std::memory_order_release);
    }

    /// Read the current value (acquire semantics).
    T
    load() const noexcept
    {
        return m_value.load(std::memory_order_acquire);
    }

private:
    std::atomic<T> m_value;
};

} // namespace clockr::conc
