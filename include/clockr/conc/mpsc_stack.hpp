#pragma once

#include <atomic>
#include <cstddef>

namespace clockr::conc
{

/// An intrusive, lock-free multi-producer/single-consumer stack.
///
/// Nodes carry their own link (`std::atomic<T*> next`) and may be pushed at
/// most once per lifetime: a node is never re-pushed after being popped, which
/// is what makes the single consumer's Treiber pop safe from ABA. Push is
/// non-blocking; pop is lock-free. Memory belongs to the caller — this is a
/// hand-off structure, not storage.
template<typename T>
class mpsc_stack
{
public:
    /// Push `node` (which carries its own `next` link). A node may be pushed
    /// at most once per lifetime and must not be re-pushed after being popped.
    void
    push(T* node) const noexcept
    {
        T* head { m_head.load(std::memory_order_relaxed) };

        do
        {
            node->next.store(head, std::memory_order_relaxed);
        }
        while (!m_head.compare_exchange_weak(
            head,
            node,
            std::memory_order_release,
            std::memory_order_relaxed));
    }

    /// Pop one node, or null when empty. Single consumer only.
    T*
    pop() const noexcept
    {
        T* head { m_head.load(std::memory_order_acquire) };

        while (head != nullptr)
        {
            T* next { head->next.load(std::memory_order_relaxed) };

            if (m_head.compare_exchange_weak(
                    head,
                    next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return head;
            }
        }

        return nullptr;
    }

    /// True when the stack holds no nodes. Single consumer only (like `pop`).
    bool
    empty() const noexcept
    {
        return m_head.load(std::memory_order_acquire) == nullptr;
    }

private:
    // The stack is a handle to shared state: push/pop mutate the link, not the
    // handle, so they are const (same call as mem::pool, which user cells also
    // hold as a const home member).
    mutable std::atomic<T*> m_head { nullptr };
};

} // namespace clockr::conc
