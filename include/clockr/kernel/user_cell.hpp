#pragma once

/// @file
/// The ref-counted pool cell behind a `user_ref` value (see `user_type.hpp`
/// and `value.hpp`).
///
/// A user struct too large for `user_inline` lives in a cell: a small intrusive
// header followed by the payload, allocated from a `mem::pool` (or the heap on
// dev/test paths). Copies of a `kernel::value` copy the *cell pointer*, never
// the payload; the kernel keeps the refcount balanced at well-defined escape
// points (delay state, ports, links, end of tick) — see the owning engine
// and doc/reference/denotational-semantics.md.
//
// Reclamation: releasing the last reference runs the payload's destructor and
// returns the memory. In G1 that happens inline; the cross-thread transport
// work routes it through the home pool's retire queue instead, so a lock-free
// reader that is mid-`retain` can never observe reused memory. The `next`
// link exists for that queue and is unused until then.

#include "clockr/kernel/clock.hpp"
#include "clockr/kernel/user_type.hpp"

#include "clockr/conc/mpsc_stack.hpp"
#include "clockr/mem/pool.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace clockr::kernel
{

/// The program's grace clock: an epoch bumped whenever a region enters a tick,
/// plus one announcement slot per region (0 = idle, else the epoch of the
/// region's in-flight tick).
///
/// A retired cell is reclaimable only when every region is idle or has
/// announced a later epoch than the cell's retirement stamp. That is the
/// grace period a lock-free `latest` reader needs: a reader that is preempted
/// between reading a slot and taking its reference keeps its region's
/// announcement behind the retirement, so the cell it may still touch is
/// withheld from reclamation until the read window closes.
struct grace_clock
{
    std::atomic<std::uint64_t> epoch { 0 };
    std::array<std::atomic<std::uint64_t>, clock_tag_count> announce {};
};

struct user_cell;

/// The owning context of a cell: the region pool that allocated it and the
/// reclamation state that returns it. A null home means the cell lives on the
/// heap (the dev/test path).
///
/// A home lives in a program-owned fixed array, never inside a movable
/// `region_runtime`: cells store a raw `const value_home*`, so the address must
/// be stable for the program's whole life.
struct value_home
{
    explicit value_home(std::size_t pool_bytes) :
        pool { mem::arena { pool_bytes } }
    {}

    value_home(const value_home&) = delete;
    value_home& operator=(const value_home&) = delete;

    mem::pool pool;

    // Cells whose last reference was dropped, awaiting reclamation. Pushed by
    // any thread, popped only by the home region (or program destruction).
    conc::mpsc_stack<user_cell> retired {};

    // Cells withheld by the current reclamation pass because a region was
    // still inside its read window; rotated back onto `retired` when the pass
    // ends. Only the home thread touches it.
    conc::mpsc_stack<user_cell> deferred {};

    // The grace clock this home's cells stamp their retirement with, and this
    // region's slot in it. Both null for a home without cross-thread
    // reclamation (heap/dev paths use null homes instead).
    grace_clock* grace { nullptr };
    std::atomic<std::uint64_t>* announce { nullptr };
};

/// A ref-counted cell holding a user value. `type` is the value's vtable;
/// `home` says where to return the memory; `next` is the retire-queue link;
/// `retire_epoch` is the grace epoch the last release stamped it with (read
/// only by the reclaiming home thread, after the queue hands the cell over).
enum class cell_state : std::uint8_t
{
    live,
    retired,
    dead,
};

struct user_cell
{
    std::atomic<std::uint32_t> refs { 1 };
    const user_type* type { nullptr };
    const value_home* home { nullptr };
    std::atomic<user_cell*> next { nullptr };
    std::uint64_t retire_epoch { 0 };

    // Lifecycle tracking: catches a double-retire or a reclaim of a cell that
    // was never retired, before the bad pointer turns into a wild call.
    cell_state state { cell_state::live };
};

/// Byte offset of the payload inside a cell, for a payload of alignment
/// `align`. Shared by allocation and payload access so the two cannot drift.
constexpr
std::size_t
user_payload_offset(std::size_t align) noexcept
{
    return (sizeof(user_cell) + align - 1) & ~(align - 1);
}

/// The payload address of a cell, as `void*`/`const void*`.
inline
void*
user_payload(user_cell* cell) noexcept
{
    return reinterpret_cast<std::byte*>(cell) + user_payload_offset(cell->type->align);
}

inline
const void*
user_payload(const user_cell* cell) noexcept
{
    return reinterpret_cast<const std::byte*>(cell) + user_payload_offset(cell->type->align);
}

/// Alignment the cell allocation must satisfy: the header's, or the payload's
/// if the payload is more strictly aligned.
constexpr
std::size_t
user_cell_align(std::size_t payload_align) noexcept
{
    return payload_align > alignof(user_cell) ? payload_align : alignof(user_cell);
}

/// Add a reference. Relaxed: the caller already has a reference, which is the
/// synchronization edge that keeps the cell alive.
inline
void
retain(user_cell* cell) noexcept
{
    if (cell != nullptr)
        cell->refs.fetch_add(1, std::memory_order_relaxed);
}

/// Add a reference only if the cell is still live; false means the last
/// reference was already dropped (the cell is retired and awaits reclamation).
///
/// This is the retain a lock-free reader uses: a blind increment would revive
/// a retired cell, and its later drop would retire it a second time. The
/// reader's memory safety comes from the grace period, which keeps a retired
/// cell's refcount readable (never reused) until every read window that could
/// reference it has closed.
inline
bool
retain_if_live(user_cell* cell) noexcept
{
    if (cell == nullptr)
        return true;

    std::uint32_t count { cell->refs.load(std::memory_order_relaxed) };

    do
    {
        if (count == 0)
            return false;
    }
    while (!cell->refs.compare_exchange_weak(
        count,
        count + 1,
        std::memory_order_relaxed,
        std::memory_order_relaxed));

    return true;
}

/// Destroy the payload and return the memory. Runs on the home thread (the
/// only thread that may touch the pool's free lists and the user's
/// destructor) when reclamation drains a retire queue.
inline
void
reclaim(user_cell* cell, mem::pool* pool) noexcept
{
    assert(cell->state == cell_state::retired && "reclaiming a cell that is not retired");
    cell->state = cell_state::dead;

    if (cell->type != nullptr && cell->type->destroy != nullptr)
        cell->type->destroy(user_payload(cell));

    if (pool != nullptr)
    {
        pool->deallocate(cell);
    }
    else
    {
        const auto align { user_cell_align(cell->type != nullptr ? cell->type->align : alignof(user_cell)) };
        ::operator delete(cell, std::align_val_t{ align });
    }
}

/// Drop a reference; on the last one, hand the cell to its home for
/// reclamation.
///
/// The releasing thread does only bounded work — an atomic decrement and a
/// lock-free push — because it may be a real-time thread. The home thread
/// destroys the payload and frees the memory later, after the grace period: a
/// lock-free reader that is mid-`retain` (see `conc::slot::load_retained`) must
/// never observe reused memory. `acq_rel` makes the final decrement acquire the
/// other threads' payload writes before the destructor eventually reads them.
inline
void
release(user_cell* cell) noexcept
{
    if (cell == nullptr)
        return;

    if (cell->refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;

    // The 1 -> 0 transition happens exactly once per cell: `retain_if_live`
    // refuses to revive a retired cell, so a second transition is a bug (an
    // extra release, or a blind retain on a cell the producer had already
    // handed to the reclaimer).
    assert(cell->state == cell_state::live && "the last reference of a retired cell was dropped again");
    cell->state = cell_state::retired;

    if (cell->home != nullptr)
    {
        cell->retire_epoch = cell->home->grace != nullptr
            ? cell->home->grace->epoch.load(std::memory_order_seq_cst)
            : 0;
        cell->home->retired.push(cell);
        return;
    }

    reclaim(cell, nullptr);
}

/// Allocate and construct a cell for `T`, with `refs == 1`.
///
/// `home` may be null (heap); a non-null home routes allocation through its
/// pool. Throws `std::bad_alloc` when the pool is exhausted — pool sizing is
/// the host's obligation (see the RT-safety notes in the docs).
template<typename T, typename... Args>
requires std::constructible_from<T, Args...>
user_cell*
make_user_cell(const value_home* home, Args&&... args)
{
    static_assert(user_value_c<T>, "make_user_cell is for user structs");

    const std::size_t payload_offset { user_payload_offset(alignof(T)) };
    const std::size_t bytes { payload_offset + sizeof(T) };
    const std::size_t align { user_cell_align(alignof(T)) };

    void* storage { nullptr };

    if (home != nullptr)
        storage = home->pool.allocate_aligned(bytes, align);
    else
        storage = ::operator new(bytes, std::align_val_t{ align });

    if (storage == nullptr)
        throw std::bad_alloc {};

    auto* cell { std::construct_at(static_cast<user_cell*>(storage)) };
    cell->type = &tag_of<T>();
    cell->home = home;

    // Braced placement-new: aggregate user structs initialize element-wise,
    // and clang rejects parenthesized aggregate initialization as a C++20
    // extension under this project's -Weverything -Werror.
    ::new (static_cast<void*>(static_cast<std::byte*>(storage) + payload_offset))
        T { std::forward<Args>(args)... };

    return cell;
}

} // namespace clockr::kernel
