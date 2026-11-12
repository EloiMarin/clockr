#include "clockr/replay/session.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace clockr::replay
{

session::session(
    kernel::program& program,
    kernel::engine& engine,
    codec_registry& codecs,
    scenario_builder& builder
) :
    m_program { program },
    m_engine { engine },
    m_codecs { codecs },
    m_builder { builder }
{
    for (std::size_t i { 0 }; i < kernel::clock_tag_count; ++i)
    {
        const auto tag { static_cast<kernel::clock_tag>(i) };

        if (m_program.region(tag).published_epoch.load(
                std::memory_order_acquire
            ) != 0)
        {
            m_builder.mark_incomplete("late start: program has already ticked");
            break;
        }
    }
}

void
session::mark_initialized()
{
    if (m_initialized)
        return;

    m_builder.mark_initialized(m_clock);
    m_initialized = true;
}

void
session::begin_epoch(kernel::clock_tag clock)
{
    m_clock = clock;
    m_builder.begin_epoch(clock);
}

void
session::set(std::string_view port, kernel::value value)
{
    apply_set(port, value);
}

void
session::apply_set(std::string_view port, const kernel::value& value)
{
    encoded_value encoded;
    std::string error;

    if (!encode_value(value, m_codecs, encoded, error))
    {
        m_builder.mark_missing(
            1, "unencodable input '" + std::string { port } + "': " + error
        );

        m_program.set(port, value);
        return;
    }

    m_builder.set_input(port, std::move(encoded), false);
    m_program.set(port, value);
}

void
session::step(kernel::clock_tag clock)
{
    const entry_id id { m_builder.step_region(clock) };

    if (!m_initialized)
        m_builder.mark_incomplete("step before initialization boundary");

    try
    {
        m_engine.step(m_program, clock);
    }
    catch (...)
    {
        m_builder.mark_threw(id);
        m_builder.mark_incomplete("throwing step");
        m_aborted = true;
        throw;
    }
}

const scenario&
session::recorded() const
{
    return m_builder.peek();
}

} // namespace clockr::replay
