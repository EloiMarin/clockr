#pragma once

#include "clockr/mem/arena.hpp"

#include <cstddef>

namespace clockr::mem
{

/// A fixed-size TLSF (two-level segregated fit) allocator inside an `arena`.
/// It never grows, never falls back to the system allocator and never blocks:
/// exhaustion throws `std::bad_alloc`.
///
/// Allocation and deallocation do not mutate the handle, so both are `const`.
class pool
{
public:
    /// The smallest arena that can be turned into a pool.
    static
    std::size_t
    minimal_size();

    /// Take ownership of `storage` and build the TLSF index inside it.
    explicit pool(arena&& storage);

    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;

    pool(pool&&) = delete;
    pool& operator=(pool&&) = delete;

    ~pool();

    /// Allocate `bytes` with the pool's default (pointer) alignment, or throw `std::bad_alloc`.
    void*
    allocate(std::size_t bytes) const;

    /// Allocate `bytes` aligned to `align`, or throw `std::bad_alloc`.
    void*
    allocate_aligned(std::size_t bytes, std::size_t align) const;

    /// Return a block previously obtained from this pool.
    void
    deallocate(void* ptr) const noexcept;

    /// True when `ptr` points inside this pool's storage.
    bool
    owns(const void* ptr) const noexcept;

    /// Verify the allocator's internal invariants (debug aid).
    bool
    check() const noexcept;

    /// A point-in-time accounting of the pool.
    struct stats
    {
        std::size_t used_bytes { 0 };
        std::size_t free_bytes { 0 };
        std::size_t overhead_bytes { 0 };
    };

    /// Snapshot the current usage; useful for sizing a region pool.
    stats
    snapshot() const;

    /// Total capacity of the underlying arena in bytes.
    std::size_t
    capacity() const noexcept
    {
        return m_storage.size();
    }

private:
    void* m_tlsf { nullptr };

    arena m_storage;
};

} // namespace clockr::mem
