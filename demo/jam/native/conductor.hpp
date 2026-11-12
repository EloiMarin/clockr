#pragma once

/// @file
/// Deterministic headless conductor host for the jam's fixed network.
///
/// The conductor owns one `kernel::program` built from `jam::net::app`, one
/// `release_engine`, and one total schedule. It advances fixed audio blocks
/// (256 frames at 48 kHz) and a UI tick every eight audio blocks:
///
///     block b:  if b > 0 and b % 8 == 0: apply one queued edit, tick ui
///               set block, tick audio, publish the rendered block
///
/// Every call to `engine::step` goes through `replay::session`, so the
/// boundary journal is the recording. Participant edits arrive on a lock-free
/// SPSC queue and are applied only between ticks, at the UI boundary. Rendered
/// blocks leave on a bounded lock-free SPSC ring for a consumer that never
/// touches the program.
///
/// The ring holds `audio_capacity` blocks (1024 by default, about 5.5 seconds
/// at 48 kHz); a full ring drops and counts the block. The consumer contract
/// is: call `pop_block` from one consumer thread (normally the audio
/// callback), do no program work there, and never block. The callback path is
/// allocation-free and lock-free.
///
/// Recording uses the complete boundary journal, not the lossy live trace.
/// Construction, codec registration and replay are off the render path; the
/// journal itself is appended by the conductor between ticks. `step_block`
/// performs that boundary recording and may allocate; only the ring consumer
/// path is guaranteed allocation-free. Do not call `finish_recording` while a
/// producer is still submitting.

#include "jam/network.hpp"
#include "jam/patch.hpp"
#include "jam/score.hpp"

#include "daw/transport.hpp"
#include "clockr/conc/spsc.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/replay/factory.hpp"
#include "clockr/replay/run.hpp"
#include "clockr/replay/scenario.hpp"
#include "clockr/replay/session.hpp"
#include "clockr/replay/value_codec.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace jam::native
{

namespace replay = clockr::replay;
namespace kernel = clockr::kernel;

/// The concrete recipe type `jam::net::app` returns.
using recipe_type = decltype(jam::net::app(jam::score {}));

/// Conductor sizing and identity.
struct conductor_options
{
    /// Room for this many pending participant edits before `submit` reports
    /// full. The queue is never drained by the producer.
    std::size_t edit_capacity { 64 };

    /// Room for this many rendered blocks before the conductor reports a drop.
    std::size_t audio_capacity { 1024 };

    /// Per-region pool headroom for score and voice user values.
    std::size_t region_pool_bytes { std::size_t { 1 } << 20 };

    /// Journal identity.
    std::uint64_t run { 1 };
    std::uint64_t branch { 1 };
};

/// One participant edit on the conductor's inbound queue. Deliberately small
/// and trivially copyable: `submit` performs no allocation.
struct participant_edit
{
    std::uint32_t participant { 0 };
    std::uint64_t revision { 0 };
    jam::score_patch patch {};
};

static_assert(std::is_trivially_copyable_v<participant_edit>);

/// One rendered audio block on the outbound ring. The consumer never reads the
/// program; `mix` and `playhead` are the snapshot the demo needs.
struct rendered_block
{
    std::uint64_t index { 0 };
    std::int64_t start_frame { 0 };
    std::int64_t frames { 0 };
    double sample_rate { 0.0 };
    double mix { 0.0 };
    double playhead { 0.0 };
};

static_assert(std::is_trivially_copyable_v<rendered_block>);

/// A deterministic digest of every captured run output, in step order. It
/// hashes encoded scalar bits and user-value bytes; addresses, durations and
/// epochs are excluded.
[[nodiscard]] std::uint64_t
run_digest(const replay::run& run) noexcept;

/// The v1 conductor: one program, one engine, one total schedule.
class conductor
{
public:
    static constexpr std::int64_t frames_per_block { 256 };
    static constexpr double sample_rate { 48000.0 };
    static constexpr std::size_t ui_every_blocks { 8 };
    static constexpr double loop_end { 1024.0 };
    static constexpr double master_level { 0.8 };

    /// Build the program and record the initialization UI boundary. Performs
    /// heap allocation and one UI tick, so never call it on an audio thread.
    explicit conductor(
        jam::score initial,
        conductor_options options = {}
    );

    conductor(const conductor&) = delete;
    conductor& operator=(const conductor&) = delete;
    conductor(conductor&&) = delete;
    conductor& operator=(conductor&&) = delete;
    ~conductor();

    /// Advance one audio block. A due UI boundary (every eight blocks, never
    /// before block 0) runs first; it applies at most one queued edit, records
    /// and ticks the UI region, then the audio block is recorded and ticked.
    void
    step_block();

    /// Call `step_block` `blocks` times.
    void
    advance(std::size_t blocks);

    /// Run one explicit UI boundary outside the fixed audio cadence. It still
    /// applies at most one queued edit before ticking. Intended for scripted
    /// tests, not for the audio path.
    void
    step_ui();

    /// Enqueue one participant edit. False means the bounded queue is full;
    /// the edit is dropped and counted, never blocked on.
    bool
    submit(const participant_edit& edit);

    /// Pop one rendered block on the consumer thread. False means the ring is
    /// empty. Does not touch the program, allocate or block.
    bool
    pop_block(rendered_block& block);

    /// The live journal as recorded so far (read-only).
    [[nodiscard]] const replay::scenario&
    recorded() const;

    /// Freeze and return the journal. Also stamps the live run with the
    /// journal's run/branch id and fidelity. Idempotent.
    const replay::scenario&
    finish_recording();

    [[nodiscard]] bool
    recording_finished() const noexcept;

    /// The outputs captured from the live steps, suitable for
    /// `replay::compare` against a fresh-program replay of `recorded()`.
    [[nodiscard]] const replay::run&
    live_run() const noexcept;

    /// `run_digest(live_run())`.
    [[nodiscard]] std::uint64_t
    digest() const noexcept;

    [[nodiscard]] const recipe_type&
    recipe() const noexcept;

    [[nodiscard]] const replay::codec_registry&
    codecs() const noexcept;

    [[nodiscard]] const conductor_options&
    options() const noexcept;

    [[nodiscard]] const jam::score&
    initial_score() const noexcept;

    [[nodiscard]] std::size_t
    edit_capacity() const noexcept;

    [[nodiscard]] std::size_t
    audio_capacity() const noexcept;

    [[nodiscard]] std::size_t
    pending_edits() const;

    [[nodiscard]] std::size_t
    pending_blocks() const;

    [[nodiscard]] std::uint64_t
    blocks_rendered() const noexcept;

    [[nodiscard]] std::uint64_t
    blocks_dropped() const noexcept;

    [[nodiscard]] std::uint64_t
    edits_applied() const noexcept;

    [[nodiscard]] std::uint64_t
    edits_dropped() const noexcept;

    [[nodiscard]] bool
    aborted() const noexcept;

private:
    void
    ensure_steps_allowed() const;

    void
    capture(kernel::clock_tag clock);

    void
    publish_block();

    jam::score m_initial;
    conductor_options m_options;
    recipe_type m_recipe;
    kernel::program m_program;
    kernel::release_engine m_engine;
    replay::codec_registry m_codecs;
    replay::scenario_builder m_builder;
    replay::session m_session;
    clockr::conc::spsc<participant_edit> m_edits;
    clockr::conc::spsc<rendered_block> m_audio;
    replay::run m_live_run;
    std::optional<replay::scenario> m_final;
    std::array<std::uint64_t, kernel::clock_tag_count> m_ordinals {};
    std::uint64_t m_block_index { 0 };
    std::int64_t m_next_frame { 0 };
    std::atomic<std::uint64_t> m_blocks_rendered { 0 };
    std::atomic<std::uint64_t> m_blocks_dropped { 0 };
    std::atomic<std::uint64_t> m_edits_applied { 0 };
    std::atomic<std::uint64_t> m_edits_dropped { 0 };
};

} // namespace jam::native
