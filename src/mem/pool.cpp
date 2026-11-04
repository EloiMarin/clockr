#include "clockr/mem/pool.hpp"

#include <tlsf.h>

#include <cstdint>
#include <new>
#include <utility>

namespace clockr::mem
{

namespace
{
    struct walk_state
    {
        std::size_t used { 0 };
        std::size_t free { 0 };
    };

    void
    accumulate(void* ptr, std::size_t size, int used, void* user);
}

std::size_t
pool::minimal_size()
{
    return tlsf_size() + tlsf_pool_overhead() + tlsf_block_size_min();
}

pool::pool(arena&& storage) :
    m_storage { std::move(storage) }
{
    if (m_storage.size() < minimal_size())
        throw std::bad_alloc {};

    m_tlsf = tlsf_create_with_pool(m_storage.data(), m_storage.size());

    if (!m_tlsf)
        throw std::bad_alloc {};

    // TLSF's create_with_pool ignores a failed add_pool, so a storage size it
    // cannot represent (above its maximum block size) leaves a valid-looking
    // control with no usable pool: every later allocation would fail on the
    // ticking thread. Probe once so that sizing mistake surfaces here, at
    // construction, where throwing is allowed.
    void* const probe { tlsf_malloc(m_tlsf, tlsf_block_size_min()) };

    if (!probe)
        throw std::bad_alloc {};

    tlsf_free(m_tlsf, probe);
}

pool::~pool()
{
    if (m_tlsf)
        tlsf_destroy(m_tlsf);
}

void*
pool::allocate(std::size_t bytes) const
{
    return tlsf_malloc(m_tlsf, bytes);
}

void*
pool::allocate_aligned(std::size_t bytes, std::size_t align) const
{
    return tlsf_memalign(m_tlsf, align, bytes);
}

void
pool::deallocate(void* ptr) const noexcept
{
    tlsf_free(m_tlsf, ptr);
}

bool
pool::owns(const void* ptr) const noexcept
{
    const auto p { reinterpret_cast<std::uintptr_t>(ptr) };
    const auto base { reinterpret_cast<std::uintptr_t>(m_storage.data()) };

    return p >= base && p < base + m_storage.size();
}

bool
pool::check() const noexcept
{
    return tlsf_check(m_tlsf) == 0 && tlsf_check_pool(tlsf_get_pool(m_tlsf)) == 0;
}

pool::stats
pool::snapshot() const
{
    walk_state state;
    tlsf_walk_pool(tlsf_get_pool(m_tlsf), accumulate, &state);

    return {
        state.used,
        state.free,
        m_storage.size() - state.used - state.free,
    };
}

namespace
{
    void
    accumulate(void* ptr, std::size_t size, int used, void* user)
    {
        (void)ptr;

        auto& state { *static_cast<walk_state*>(user) };

        if (used)
            state.used += size;
        else
            state.free += size;
    }
}

} // namespace clockr::mem
