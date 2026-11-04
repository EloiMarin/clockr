#include "clockr/mem/arena.hpp"

#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace clockr::mem
{

namespace
{
    std::size_t
    round_up(std::size_t value, std::size_t multiple);
}

arena::arena(std::size_t bytes) :
    m_bytes { round_up(bytes, alignment) }
{
#if defined(_WIN32)
    m_storage = _aligned_malloc(m_bytes, alignment);
#else
    m_storage = std::aligned_alloc(alignment, m_bytes);
#endif

    if (!m_storage)
        throw std::bad_alloc {};
}

arena::~arena()
{
    release();
}

arena::arena(arena&& rhs) noexcept :
    m_storage { rhs.m_storage },
    m_bytes { rhs.m_bytes }
{
    rhs.m_storage = nullptr;
    rhs.m_bytes = 0;
}

arena&
arena::operator=(arena&& rhs) noexcept
{
    if (this != &rhs)
    {
        release();

        m_storage = rhs.m_storage;
        m_bytes = rhs.m_bytes;

        rhs.m_storage = nullptr;
        rhs.m_bytes = 0;
    }

    return *this;
}

void
arena::release() noexcept
{
    if (!m_storage)
        return;

#if defined(_WIN32)
    _aligned_free(m_storage);
#else
    std::free(m_storage);
#endif

    m_storage = nullptr;
    m_bytes = 0;
}

namespace
{
    std::size_t
    round_up(std::size_t value, std::size_t multiple)
    {
        return (value + multiple - 1) / multiple * multiple;
    }
}

} // namespace clockr::mem
