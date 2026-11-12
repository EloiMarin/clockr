#pragma once

/// @file
/// Jam session: the authoritative, ordered action log over the fixed network.
///
/// The session keeps one canonical `replay::scenario` plus application
/// metadata (participant, base revision, review decisions). A proposal is
/// validated and appended as a score-patch boundary event; the scenario is
/// rebuilt deterministically so `history()` is always a real, replayable
/// journal. There are no CRDTs and no silent rebasing: a proposal against a
/// stale base is rejected.
///
/// `branch_id.value` is a revision number: revision 0 is the raw initial
/// head (no patches), and the head revision is the largest recorded. A render
/// beyond the head is clamped to the head; `render(head)` reports the full
/// fidelity of the canonical history.

#include "jam/patch.hpp"
#include "jam/scheduler.hpp"
#include "jam/score.hpp"
#include "jam/voice.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/value.hpp"
#include "clockr/replay/compare.hpp"
#include "clockr/replay/factory.hpp"
#include "clockr/replay/replayer.hpp"
#include "clockr/replay/scenario.hpp"
#include "clockr/replay/value_codec.hpp"
#include "daw/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jam
{

namespace replay = clockr::replay;
namespace kernel = clockr::kernel;

/// One accepted revision number.
struct revision
{
    std::uint64_t value { 0 };

    friend bool
    operator==(const revision&, const revision&) = default;
};

/// One participant identity.
struct participant
{
    std::string id;
};

/// One accepted proposal in the canonical log.
struct action
{
    revision rev {};
    revision base {};
    participant author;
    score_patch patch;
};

/// One review decision attached to a revision.
struct review
{
    revision target {};
    std::string note;
};

class session
{
public:
    /// Fixed deterministic block timing for the journal.
    static constexpr std::int64_t block_frames { 256 };
    static constexpr double block_rate { 48000.0 };

    /// Large enough that ordinary test/performance histories never wrap.
    static constexpr double loop_end { 1024.0 };

    /// Initial master level used by the standard initialization.
    static constexpr double master_level { 0.8 };

    session(
        score initial,
        replay::program_factory& factory,
        replay::codec_registry& codecs
    ) :
        m_initial { std::move(initial) },
        m_score { m_initial },
        m_factory { factory },
        m_codecs { codecs }
    {
        m_codecs.register_user<jam::score_patch>("jam.score_patch");
        m_codecs.register_user<jam::event_batch>("jam.event_batch");
        m_codecs.register_user<jam::voice_state>("jam.voice_state");
        m_codecs.register_user<daw::LoopRegion>("daw.loop_region");
        m_codecs.register_user<daw::BlockDesc>("daw.block_desc");

        rebuild_history();
    }

    /// Validate and append a proposal. Rejected proposals leave the log and
    /// head revision untouched and explain why in `error`.
    bool
    propose(
        participant who,
        revision base,
        score_patch patch,
        std::string& error
    )
    {
        error.clear();

        if (m_head.value == std::numeric_limits<std::uint64_t>::max())
        {
            error = "revision space exhausted";
            return false;
        }

        if (base.value != m_head.value)
        {
            error = "stale base revision " + std::to_string(base.value) +
                    " (head is " + std::to_string(m_head.value) + ")";
            return false;
        }

        if (!validate(patch, error))
            return false;

        m_actions.push_back(
            action {
                revision { m_head.value + 1 }, base, std::move(who), patch }
        );
        m_score = jam::apply(m_score, patch);
        ++m_head.value;

        rebuild_history();

        return true;
    }

    /// Record a review decision. The mark is appended to the history and never
    /// rewrites an audible past action.
    void
    accept(revision rev)
    {
        if (rev.value > m_head.value)
            return;

        m_reviews.push_back(
            review { rev, "accept revision " + std::to_string(rev.value) }
        );

        rebuild_history();
    }

    /// The current canonical, replayable history.
    [[nodiscard]] const replay::scenario&
    history() const
    {
        return m_history;
    }

    /// Replay a revision into a fresh program. Never mutates the log.
    [[nodiscard]] replay::run
    render(replay::branch_id branch) const
    {
        std::uint64_t effective { branch.value };

        if (effective > m_head.value)
            effective = m_head.value;

        const auto scenario { build(effective) };

        replay::replayer replayer;
        replay::replay_options options;
        options.capture_outputs = true;
        options.capture_trace = false;
        options.program_options.region_pool_bytes = std::size_t { 1 } << 20;

        return replayer.replay(scenario, m_factory, m_codecs, options);
    }

    /// Compare two revisions semantically by replaying both.
    [[nodiscard]] replay::run_diff
    compare(replay::branch_id lhs, replay::branch_id rhs) const
    {
        return replay::compare(render(lhs), render(rhs));
    }

    /// The largest recorded revision (0 before any proposal).
    [[nodiscard]] revision
    head() const noexcept
    {
        return m_head;
    }

    /// Accepted proposals, in order.
    [[nodiscard]] const std::vector<action>&
    actions() const noexcept
    {
        return m_actions;
    }

    /// Review decisions, in call order.
    [[nodiscard]] const std::vector<review>&
    reviews() const noexcept
    {
        return m_reviews;
    }

    /// The score after all accepted patches.
    [[nodiscard]] const score&
    current_score() const noexcept
    {
        return m_score;
    }

private:
    [[nodiscard]] static bool
    validate(const score_patch& patch, std::string& error)
    {
        const auto count { static_cast<std::size_t>(patch.count) };

        if (count > patch_capacity)
        {
            error = "patch count " + std::to_string(count) +
                    " exceeds capacity " + std::to_string(patch_capacity);
            return false;
        }

        const auto require_pattern =
            [&error](std::uint8_t index, const char* field) {
                if (index < pattern_count)
                    return true;

                error = std::string { "patch " } + field + " index " +
                        std::to_string(index) + " is out of range";
                return false;
            };

        for (std::size_t i { 0 }; i < count; ++i)
        {
            const auto& op { patch.ops[i] };
            const auto kind { static_cast<std::uint8_t>(op.op) };

            if (kind > static_cast<std::uint8_t>(patch_op::sequence))
            {
                error = "patch op " + std::to_string(kind) + " is unknown";
                return false;
            }

            switch (op.op)
            {
                case patch_op::set_step:
                    if (!require_pattern(op.a, "pattern") ||
                        op.b >= steps_per_pattern)
                    {
                        error = "patch step index " + std::to_string(op.b) +
                                " is out of range";
                        return false;
                    }
                    if (op.c > 127)
                    {
                        error = "patch note " + std::to_string(op.c) +
                                " is out of range";
                        return false;
                    }
                    if (op.d < 0 || op.d > 127)
                    {
                        error = "patch velocity " + std::to_string(op.d) +
                                " is out of range";
                        return false;
                    }
                    break;

                case patch_op::clear_step:
                    if (!require_pattern(op.a, "pattern") ||
                        op.b >= steps_per_pattern)
                    {
                        error = "patch step index " + std::to_string(op.b) +
                                " is out of range";
                        return false;
                    }
                    break;

                case patch_op::transpose:
                    if (!require_pattern(op.a, "pattern"))
                        return false;
                    if (op.d < -127 || op.d > 127)
                    {
                        error = "patch transpose " + std::to_string(op.d) +
                                " is out of range";
                        return false;
                    }
                    break;

                case patch_op::rotate:
                    if (!require_pattern(op.a, "pattern"))
                        return false;
                    break;

                case patch_op::mask:
                    if (!require_pattern(op.a, "pattern"))
                        return false;
                    break;

                case patch_op::density:
                    if (!require_pattern(op.a, "pattern"))
                        return false;
                    if (op.c == 0)
                    {
                        error = "patch density period is zero";
                        return false;
                    }
                    break;

                case patch_op::stack:
                case patch_op::sequence:
                    if (!require_pattern(op.a, "destination pattern") ||
                        !require_pattern(op.b, "source pattern"))
                        return false;
                    break;

                default:
                    error = "patch op is not supported";
                    return false;
            }
        }

        return true;
    }

    static void
    set_value(
        replay::scenario_builder& builder,
        const replay::codec_registry& codecs,
        std::string_view port,
        const kernel::value& value,
        bool is_event
    )
    {
        replay::encoded_value encoded;
        std::string error;

        if (!replay::encode_value(value, codecs, encoded, error))
            throw std::runtime_error { "jam::session cannot encode input '" +
                                       std::string { port } + "': " + error };

        builder.set_input(port, std::move(encoded), is_event);
    }

    template<typename T, typename... Args>
    static void
    set_user(
        replay::scenario_builder& builder,
        const replay::codec_registry& codecs,
        std::string_view port,
        bool is_event,
        Args&&... args
    )
    {
        const kernel::value value {
            kernel::make_user_value<T>(nullptr, std::forward<Args>(args)...)
        };

        replay::encoded_value encoded;
        std::string error;
        const bool ok { replay::encode_value(value, codecs, encoded, error) };

        kernel::release(value);

        if (!ok)
            throw std::runtime_error { "jam::session cannot encode input '" +
                                       std::string { port } + "': " + error };

        builder.set_input(port, std::move(encoded), is_event);
    }

    [[nodiscard]] replay::scenario
    build(std::uint64_t upto) const
    {
        replay::scenario_builder builder {
            replay::run_id { 1 },
            replay::branch_id { upto },
        };

        builder.begin_epoch(kernel::clock_tag::ui);
        builder.mark_initialized(kernel::clock_tag::ui);
        set_value(builder, m_codecs, "playing", kernel::value { true }, false);
        set_value(
            builder,
            m_codecs,
            "bpm",
            kernel::value { static_cast<double>(m_initial.bpm) },
            false
        );
        set_user<daw::LoopRegion>(
            builder,
            m_codecs,
            "loop",
            false,
            daw::LoopRegion { 0.0, loop_end, true }
        );
        set_value(
            builder, m_codecs, "master", kernel::value { master_level }, false
        );
        builder.step_region(kernel::clock_tag::ui);

        std::uint64_t block { 0 };

        for (const auto& accepted : m_actions)
        {
            if (accepted.rev.value > upto)
                break;

            builder.mark(
                "revision " + std::to_string(accepted.rev.value) + " by " +
                accepted.author.id + " base " +
                std::to_string(accepted.base.value)
            );

            builder.begin_epoch(kernel::clock_tag::ui);
            set_user<jam::score_patch>(
                builder, m_codecs, "patch", true, accepted.patch
            );
            builder.step_region(kernel::clock_tag::ui);

            builder.begin_epoch(kernel::clock_tag::audio);
            set_user<daw::BlockDesc>(
                builder,
                m_codecs,
                "block",
                false,
                daw::BlockDesc {
                    static_cast<std::int64_t>(block) * block_frames,
                    block_frames,
                    block_rate,
                }
            );
            builder.step_region(kernel::clock_tag::audio);

            ++block;
        }

        // One trailing audio tick makes the last patch observable even when
        // the caller renders only the log.
        builder.begin_epoch(kernel::clock_tag::audio);
        set_user<daw::BlockDesc>(
            builder,
            m_codecs,
            "block",
            false,
            daw::BlockDesc {
                static_cast<std::int64_t>(block) * block_frames,
                block_frames,
                block_rate,
            }
        );
        builder.step_region(kernel::clock_tag::audio);

        for (const auto& decision : m_reviews)
            if (decision.target.value <= upto)
                builder.mark(decision.note);

        return builder.finish();
    }

    void
    rebuild_history()
    {
        m_history = build(m_head.value);
    }

    score m_initial;
    score m_score;
    replay::program_factory& m_factory;
    replay::codec_registry& m_codecs;
    std::vector<action> m_actions;
    std::vector<review> m_reviews;
    revision m_head {};
    replay::scenario m_history;
};

} // namespace jam
