#pragma once

#include "clockr/mem/pool.hpp"

#include <cstddef>
#include <new>

namespace clockr::mem
{

/// A standard-library allocator backed by a `mem::pool`, so STL containers
/// draw from the same real-time storage as the engine's values. Allocation
/// honours `alignof(T)` and throws `std::bad_alloc` when the pool is
/// exhausted (an allocator may not return null).
template<typename T>
class pool_allocator
{
public:
    using value_type = T;

    /// Adapt `storage` to the allocator interface.
    explicit pool_allocator(pool& storage) noexcept :
        m_pool { &storage }
    {}

    /// Rebinding constructor: two allocators compare equal iff they use the same pool.
    template<typename U>
    pool_allocator(const pool_allocator<U>& rhs) noexcept :
        m_pool { rhs.m_pool }
    {}

    // The typed layer is where alignment is honored: raw pool::allocate only
    // promises pointer alignment (TLSF granularity follows pointer size), so
    // a T needing more routes through memalign explicitly.
    //
    // Throws std::bad_alloc when the pool is exhausted: an allocator may not
    // return a null pointer, and a null here would be constructed into and
    // crash. Pool sizing is the application's obligation (see program_options).
    /// Allocate storage for `count` objects of `T`.
    T*
    allocate(std::size_t count)
    {
        void* storage { m_pool->allocate_aligned(count * sizeof(T), alignof(T)) };

        if (storage == nullptr)
            throw std::bad_alloc {};

        return static_cast<T*>(storage);
    }

    /// Return storage obtained from `allocate`.
    void
    deallocate(T* ptr, std::size_t) noexcept
    {
        m_pool->deallocate(ptr);
    }

    /// The backing pool.
    pool&
    pool_ref() const noexcept
    {
        return *m_pool;
    }

    template<typename U>
    friend class pool_allocator;

private:
    pool* m_pool;
};

template<typename T, typename U>
bool
operator==(const pool_allocator<T>& lhs, const pool_allocator<U>& rhs) noexcept
{
    return &lhs.pool_ref() == &rhs.pool_ref();
}

template<typename T, typename U>
bool
operator!=(const pool_allocator<T>& lhs, const pool_allocator<U>& rhs) noexcept
{
    return !(lhs == rhs);
}

} // namespace clockr::mem
