#include "conductor.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace jam::native
{

namespace
{

kernel::program_options
make_program_options(const conductor_options& options)
{
    kernel::program_options result;
    result.region_pool_bytes = options.region_pool_bytes;

    return result;
}

constexpr std::uint64_t kFNVOffset { 0xcbf29ce484222325ULL };
constexpr std::uint64_t kFNVPrime { 0x100000001b3ULL };

void
hash_uint64(std::uint64_t& hash, std::uint64_t value) noexcept
{
    for (std::uint32_t byte { 0 }; byte < 8; ++byte)
    {
        hash ^= (value >> (byte * 8U)) & 0xffU;
        hash *= kFNVPrime;
    }
}

void
hash_bytes(std::uint64_t& hash, const std::byte* data, std::size_t count) noexcept
{
    for (std::size_t i { 0 }; i < count; ++i)
    {
        hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[i]));
        hash *= kFNVPrime;
    }
}

void
hash_encoded(std::uint64_t& hash, const replay::encoded_value& value) noexcept
{
    hash_uint64(hash, static_cast<std::uint64_t>(value.payload.index()));

    if (const auto* boolean { std::get_if<bool>(&value.payload) })
    {
        hash_uint64(hash, *boolean ? 1U : 0U);
        return;
    }

    if (const auto* integer { std::get_if<std::int64_t>(&value.payload) })
    {
        hash_uint64(hash, std::bit_cast<std::uint64_t>(*integer));
        return;
    }

    if (const auto* real { std::get_if<double>(&value.payload) })
    {
        hash_uint64(hash, std::bit_cast<std::uint64_t>(*real));
        return;
    }

    if (const auto* user { std::get_if<replay::user_bytes>(&value.payload) })
    {
        hash_uint64(hash, static_cast<std::uint64_t>(user->type_id));
        hash_uint64(hash, static_cast<std::uint64_t>(user->bytes.size()));
        hash_bytes(hash, user->bytes.data(), user->bytes.size());
        return;
    }
}

} // namespace

std::uint64_t
run_digest(const replay::run& run) noexcept
{
    std::uint64_t hash { kFNVOffset };

    hash_uint64(hash, static_cast<std::uint64_t>(run.steps.size()));

    for (const auto& step : run.steps)
    {
        hash_uint64(
            hash,
            static_cast<std::uint64_t>(kernel::clock_tag_index(step.step.clock))
        );
        hash_uint64(hash, step.step.ordinal);
        hash_uint64(hash, static_cast<std::uint64_t>(step.outputs.size()));

        for (const auto& output : step.outputs)
            hash_encoded(hash, output);
    }

    return hash;
}

conductor::conductor(jam::score initial, conductor_options options) :
    m_initial { std::move(initial) },
    m_options { options },
    m_recipe { jam::net::app(m_initial) },
    m_program { m_recipe, make_program_options(m_options) },
    m_engine {},
    m_codecs {},
    m_builder {
        replay::run_id { m_options.run },
        replay::branch_id { m_options.branch },
    },
    m_session { m_program, m_engine, m_codecs, m_builder },
    m_edits { m_options.edit_capacity },
    m_audio { m_options.audio_capacity }
{
    m_codecs.register_user<jam::score_patch>("jam.score_patch");
    m_codecs.register_user<jam::event_batch>("jam.event_batch");
    m_codecs.register_user<jam::voice_state>("jam.voice_state");
    m_codecs.register_user<daw::LoopRegion>("daw.loop_region");
    m_codecs.register_user<daw::BlockDesc>("daw.block_desc");

    m_session.begin_epoch(kernel::clock_tag::ui);
    m_session.mark_initialized();
    m_session.set("playing", kernel::value { true });
    m_session.set(
        "bpm", kernel::value { static_cast<double>(m_initial.bpm) }
    );
    m_session.set<daw::LoopRegion>("loop", 0.0, loop_end, true);
    m_session.set("master", kernel::value { master_level });
    m_session.step(kernel::clock_tag::ui);
    capture(kernel::clock_tag::ui);
}

conductor::~conductor() = default;

void
conductor::ensure_steps_allowed() const
{
    if (m_final.has_value())
        throw std::logic_error { "conductor recording is finished" };
}

void
conductor::step_block()
{
    ensure_steps_allowed();

    if (m_block_index > 0 && m_block_index % ui_every_blocks == 0)
        step_ui();

    m_session.begin_epoch(kernel::clock_tag::audio);
    m_session.set<daw::BlockDesc>(
        "block", m_next_frame, frames_per_block, sample_rate
    );
    m_session.step(kernel::clock_tag::audio);
    capture(kernel::clock_tag::audio);

    publish_block();

    m_next_frame += frames_per_block;
    ++m_block_index;
}

void
conductor::advance(std::size_t blocks)
{
    for (std::size_t i { 0 }; i < blocks; ++i)
        step_block();
}

void
conductor::step_ui()
{
    ensure_steps_allowed();

    participant_edit edit;
    const bool has_edit { m_edits.receive(edit) };

    m_session.begin_epoch(kernel::clock_tag::ui);

    if (has_edit)
    {
        m_builder.mark(
            "edit participant=" + std::to_string(edit.participant) +
            " revision=" + std::to_string(edit.revision)
        );
        m_session.set<jam::score_patch>("patch", edit.patch);
        m_edits_applied.fetch_add(1, std::memory_order_relaxed);
    }

    m_session.step(kernel::clock_tag::ui);
    capture(kernel::clock_tag::ui);
}

bool
conductor::submit(const participant_edit& edit)
{
    if (m_edits.send(edit))
        return true;

    m_edits_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool
conductor::pop_block(rendered_block& block)
{
    return m_audio.receive(block);
}

const replay::scenario&
conductor::recorded() const
{
    if (m_final.has_value())
        return *m_final;

    return m_session.recorded();
}

const replay::scenario&
conductor::finish_recording()
{
    if (!m_final.has_value())
    {
        m_final = m_builder.finish();

        m_live_run.run = m_final->run;
        m_live_run.branch = m_final->branch;
        m_live_run.fidelity = m_final->fidelity;
        m_live_run.aborted = m_session.aborted();
    }

    return *m_final;
}

bool
conductor::recording_finished() const noexcept
{
    return m_final.has_value();
}

const replay::run&
conductor::live_run() const noexcept
{
    return m_live_run;
}

std::uint64_t
conductor::digest() const noexcept
{
    return run_digest(m_live_run);
}

const recipe_type&
conductor::recipe() const noexcept
{
    return m_recipe;
}

const replay::codec_registry&
conductor::codecs() const noexcept
{
    return m_codecs;
}

const conductor_options&
conductor::options() const noexcept
{
    return m_options;
}

const jam::score&
conductor::initial_score() const noexcept
{
    return m_initial;
}

std::size_t
conductor::edit_capacity() const noexcept
{
    return m_edits.capacity();
}

std::size_t
conductor::audio_capacity() const noexcept
{
    return m_audio.capacity();
}

std::size_t
conductor::pending_edits() const
{
    return m_edits.size();
}

std::size_t
conductor::pending_blocks() const
{
    return m_audio.size();
}

std::uint64_t
conductor::blocks_rendered() const noexcept
{
    return m_blocks_rendered.load(std::memory_order_relaxed);
}

std::uint64_t
conductor::blocks_dropped() const noexcept
{
    return m_blocks_dropped.load(std::memory_order_relaxed);
}

std::uint64_t
conductor::edits_applied() const noexcept
{
    return m_edits_applied.load(std::memory_order_relaxed);
}

std::uint64_t
conductor::edits_dropped() const noexcept
{
    return m_edits_dropped.load(std::memory_order_relaxed);
}

bool
conductor::aborted() const noexcept
{
    return m_session.aborted();
}

void
conductor::capture(kernel::clock_tag clock)
{
    const auto index { kernel::clock_tag_index(clock) };

    replay::step_record record;
    record.step = replay::region_step_id { clock, m_ordinals[index]++ };
    record.epoch = m_program.region(clock).published_epoch.load(
        std::memory_order_acquire
    );

    const auto output_count { m_program.descriptor().output_ids.size() };
    record.outputs.reserve(output_count);

    for (std::size_t i { 0 }; i < output_count; ++i)
    {
        replay::encoded_value encoded;
        std::string error;

        if (!replay::encode_value(m_program.get(i), m_codecs, encoded, error))
            throw std::runtime_error {
                "conductor cannot encode output " + std::to_string(i) + ": " +
                error
            };

        record.outputs.push_back(std::move(encoded));
    }

    m_live_run.steps.push_back(std::move(record));
}

void
conductor::publish_block()
{
    rendered_block block;
    block.index = m_block_index;
    block.start_frame = m_next_frame;
    block.frames = frames_per_block;
    block.sample_rate = sample_rate;
    block.mix = m_program.get_or<double>("mix", 0.0);
    block.playhead = m_program.get_or<double>("playhead", 0.0);

    if (m_audio.send(block))
        m_blocks_rendered.fetch_add(1, std::memory_order_relaxed);
    else
        m_blocks_dropped.fetch_add(1, std::memory_order_relaxed);
}

} // namespace jam::native
