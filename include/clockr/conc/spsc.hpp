#pragma once

#include <readerwriterqueue.h>

#include <cstddef>
#include <utility>

namespace clockr::conc
{

/// A bounded single-producer/single-consumer FIFO (a thin wrapper over
/// moodycamel's lock-free queue). It never blocks; a full queue makes `send`
/// return false.
template<typename T>
class spsc
{
public:
    /// Build a queue with room for `capacity` entries.
    explicit spsc(std::size_t capacity = 1024) :
        m_queue { capacity },
        m_capacity { capacity }
    {}

    /// Enqueue by copy; false when the queue is full.
    bool
    send(const T& value)
    {
        if (m_queue.size_approx() >= m_capacity)
            return false;

        return m_queue.try_enqueue(value);
    }

    /// Enqueue by move; false when the queue is full.
    bool
    send(T&& value)
    {
        if (m_queue.size_approx() >= m_capacity)
            return false;

        return m_queue.try_enqueue(std::move(value));
    }

    /// Dequeue into `value`; false when the queue is empty.
    bool
    receive(T& value)
    {
        return m_queue.try_dequeue(value);
    }

    /// Borrow the head without dequeueing, or null when empty. Only the
    /// consumer may call this while a producer is active.
    const T*
    peek() const
    {
        return m_queue.peek();
    }

    /// Approximate number of queued entries.
    std::size_t
    size() const
    {
        return m_queue.size_approx();
    }

    /// The configured capacity.
    std::size_t
    capacity() const noexcept
    {
        return m_capacity;
    }

private:
    moodycamel::ReaderWriterQueue<T> m_queue;
    std::size_t m_capacity;
};

} // namespace clockr::conc
