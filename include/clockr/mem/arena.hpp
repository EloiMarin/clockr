#pragma once

#include <cstddef>

namespace clockr::mem
{

/// Owns a single aligned slab of memory, sized up front.
class arena
{
public:
    /// Acquire an aligned slab of `bytes` (throws `std::bad_alloc` on failure).
    explicit arena(std::size_t bytes);

    ~arena();

    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;

    arena(arena&& rhs) noexcept;
    arena& operator=(arena&& rhs) noexcept;

    /// Base address of the slab.
    void*
    data() const noexcept
    {
        return m_storage;
    }

    /// Size of the slab in bytes.
    std::size_t
    size() const noexcept
    {
        return m_bytes;
    }

    /// Alignment of the slab's base address.
    static constexpr std::size_t alignment { 64 };

private:
    void
    release() noexcept;

    void* m_storage { nullptr };
    std::size_t m_bytes { 0 };
};

} // namespace clockr::mem
