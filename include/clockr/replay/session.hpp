#pragma once

/// @file
/// Boundary recorder: records effective inputs and region steps while applying
/// them to a live program, and ticks with the ordinary engine path.

#include "clockr/replay/scenario.hpp"
#include "clockr/replay/value_codec.hpp"

#include "clockr/kernel/engine.hpp"
#include "clockr/kernel/program.hpp"

#include <string_view>
#include <utility>

namespace clockr::replay
{

/// Records one authoritative boundary schedule for a program.
///
/// One session per schedule; it is not thread-safe. Construct it before the
/// program ticks: a session started after a tick marks the journal incomplete.
class session
{
public:
    session(
        kernel::program& program,
        kernel::engine& engine,
        codec_registry& codecs,
        scenario_builder& builder
    );

    /// Record the initialization boundary.
    void
    mark_initialized();

    /// Open a new input group on `clock`.
    void
    begin_epoch(kernel::clock_tag clock);

    /// Encode and apply one erased input value.
    void
    set(std::string_view port, kernel::value value);

    /// Construct, encode and apply an owning user-struct input.
    template<typename T, typename... Args>
        requires kernel::user_value_c<T>
    void
    set(std::string_view port, Args&&... args)
    {
        const kernel::value constructed { kernel::make_user_value<T>(
            m_program.region(m_clock).home, std::forward<Args>(args)...
        ) };

        apply_set(port, constructed);
        kernel::release(constructed);
    }

    /// Record the step, then tick. A throwing tick marks the journal incomplete
    /// and rethrows.
    void
    step(kernel::clock_tag clock);

    /// The scenario recorded so far.
    const scenario&
    recorded() const;

    /// True when a recorded step threw.
    bool
    aborted() const noexcept
    {
        return m_aborted;
    }

private:
    void
    apply_set(std::string_view port, const kernel::value& value);

    kernel::program& m_program;
    kernel::engine& m_engine;
    codec_registry& m_codecs;
    scenario_builder& m_builder;
    kernel::clock_tag m_clock { kernel::clock_tag::audio };
    bool m_initialized { false };
    bool m_aborted { false };
};

} // namespace clockr::replay
