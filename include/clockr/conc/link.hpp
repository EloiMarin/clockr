#pragma once

#include "clockr/conc/slot.hpp"
#include "clockr/conc/spsc.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace clockr::conc
{

/// How a cross-clock link resamples between producer and consumer ticks.
enum class policy
{
    latest, ///< The consumer sees the newest value at or before its cut.
    queued, ///< The consumer sees every value, in order (bounded queue).
};

template<typename T, policy P>
class link;

/// A producer-tick version, used to publish several channels at one atomic
/// cut (§3.2). Counts completed producer ticks; 0 means "nothing published
/// yet". 64-bit so wrap is not a practical concern at audio rates.
using epoch_id = std::uint64_t;

/// A single-producer/multi-consumer ring that carries the value of each
/// producer epoch. A consumer snapshots one published epoch and reads the
/// newest entry at or before it, so every channel from one producer agrees on
/// the same cut.
template<typename T>
requires std::is_trivially_copyable_v<T>
class link<T, policy::latest>
{
public:
    /// A ring entry: the epoch that published it and the payload.
    struct stamped
    {
        epoch_id epoch { 0 };
        T value {};
    };

    static_assert(std::is_trivially_copyable_v<stamped>);

    /// Ring of recent epochs, so a consumer holding a stale cut can still find
    /// it after the producer moved on. Silent epochs write nothing, so the
    /// last defined value stays retained indefinitely (hold).
    static constexpr std::size_t history { 8 };

    /// Build a link whose ring is initialized to `initial`.
    explicit link(T initial = {}) noexcept
    {
        for (auto& s : m_slots)
            s.store(stamped{ 0, initial });
    }

    /// Publish one epoch's value (at most one per epoch; same-epoch repeats
    /// overwrite — last wins, per L9a). `epoch` must be > 0. Single-producer
    /// only.
    ///
    /// Each ring slot owns one reference per stored value. This overload hands
    /// the value the target slot is displacing to `release_displaced` *after*
    /// the store — releasing after the store is what lets a concurrent reader
    /// validate its snapshot. The caller retains anything the slot should keep
    /// before calling. The displaced payload is never dereferenced, only
    /// released.
    template<typename release_fn>
    void
    send(const T& value, epoch_id epoch, release_fn&& release_displaced) noexcept
    {
        assert(epoch > 0);

        slot<stamped>& target { m_slots[epoch % history] };

        // The producer is the slot's only writer; this read races only with
        // readers, which is safe.
        const stamped displaced { target.load() };

        target.store(stamped{ epoch, value });

        release_displaced(displaced.value);
    }

    /// Publish without lifetime management (scalar payloads): the same store,
    /// no displaced value to release.
    void
    send(const T& value, epoch_id epoch) noexcept
    {
        send(value, epoch, [](const T&) noexcept {});
    }


    /// Drop every reference the ring holds. Destruction-only: assumes no
    /// reader is in flight (the program destroys its links only once every
    /// region has stopped ticking). `release` takes the payload.
    template<typename release_fn>
    void
    release_all(release_fn&& release) const noexcept
    {
        for (const auto& s : m_slots)
            release(s.load().value);
    }

    /// Newest defined value with epoch <= `published`. Returns `nullopt` when
    /// the snapshot is so stale the ring has lapped it (the producer ran
    /// `history` or more defined epochs ahead between snapshot and scan) — the
    /// caller must re-snapshot and retry so all channels share one cut. Call
    /// with a `published` snapshotted once per consumer tick per producer
    /// region.
    std::optional<T>
    receive(epoch_id published) const noexcept
    {
        return receive(
            published,
            [](const T&) noexcept { return true; },
            [](const T&) noexcept {});
    }

    /// Reference-aware receive: each slot read takes a reference via `retain`
    /// *inside* the slot's version window, so a concurrent producer displacing
    /// the value cannot free it under the read; the caller receives an owned
    /// value. Slots the scan passes over are released again. `retain` returns
    /// false when the payload is already dead (the read is abandoned and
    /// retried); `release` must invert a successful retain. Both take the
    /// payload — the stamp is the link's business — and are trivial for scalar
    /// payloads.
    template<typename retain_fn, typename release_fn>
    std::optional<T>
    receive(epoch_id published, retain_fn&& retain_value, release_fn&& release_value) const noexcept
    {
        const auto retain = [&retain_value](const stamped& s) noexcept { return retain_value(s.value); };
        const auto release = [&release_value](const stamped& s) noexcept { release_value(s.value); };

        stamped best { 0, T {} };
        epoch_id newest { 0 };
        bool found { false };

        for (const auto& s : m_slots)
        {
            const stamped cur { s.load_retained(retain, release) };

            if (cur.epoch > newest)
                newest = cur.epoch;

            if (cur.epoch <= published && (!found || cur.epoch > best.epoch))
            {
                if (found)
                    release(best);

                best = cur;
                found = true;
            }
            else
            {
                release(cur);
            }
        }

        if (!found)
            return std::nullopt;

        // The scan is not atomic across slots: the producer may reuse a slot
        // between two of the reads above. If any slot has advanced a full ring
        // past the selected entry, an entry newer than it (but still at or
        // before the snapshot) may have been overwritten between two reads —
        // the scan could silently return an older value, and two channels from
        // one producer would then disagree at the same cut. Report stale so
        // the caller re-snapshots and re-reads the whole fill.
        if (newest >= best.epoch + history)
        {
            release(best);
            return std::nullopt;
        }

        return best.value;
    }

private:
    std::array<slot<stamped>, history> m_slots;
};

/// A bounded single-producer/single-consumer FIFO that carries every defined
/// epoch in order. A full queue drops the send and reports it; the capacity
/// precondition is the application's (law L6).
template<typename T>
class link<T, policy::queued>
{
public:
    /// A queue entry: the epoch that published it and the payload.
    struct stamped
    {
        epoch_id epoch { 0 };
        T value {};
    };

    /// Build a queue with room for `capacity` entries.
    explicit link(std::size_t capacity = 1024) :
        m_queue { capacity }
    {}

    /// Enqueue one epoch's value. At most one send per epoch per source (the
    /// engine guarantees this; direct users must too). Returns false when the
    /// queue is full (values are dropped).
    bool
    send(const T& value, epoch_id epoch)
    {
        assert(epoch > 0);

        return m_queue.send(stamped{ epoch, value });
    }

    bool
    send(T&& value, epoch_id epoch)
    {
        assert(epoch > 0);

        return m_queue.send(stamped{ epoch, std::move(value) });
    }

    /// Dequeue the head iff its epoch is published (`head.epoch <=
    /// published`). Future heads (sent but not yet published by the producer's
    /// version bump) are left in place.
    bool
    receive(T& value, epoch_id published)
    {
        const stamped* head { m_queue.peek() };

        if (!head)
            return false;

        if (head->epoch > published)
            return false;

#ifndef NDEBUG
        const stamped ready { *head };
#endif

        stamped entry{};

        if (!m_queue.receive(entry))
            return false;

        assert(entry.epoch == ready.epoch && "SPSC peek/dequeue mismatch");

        value = entry.value;

        return true;
    }

private:
    spsc<stamped> m_queue;
};

} // namespace clockr::conc
