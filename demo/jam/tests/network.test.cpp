// jam network — golden streams, patch epochs, permutation invariance, voice
// policy, capacity, engine parity, and an allocation-free evaluation pin.

#include "jam/network.hpp"
#include "jam/scheduler.hpp"
#include "jam/voice.hpp"

#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/shell/descriptor_storage.hpp"

#include "fixtures.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{

constexpr double kRate { 48000.0 };
constexpr std::int64_t kBeatBlock { 24000 }; // one beat at 120 bpm
constexpr std::int64_t kStepBlock { 6000 };  // one 16th at 120 bpm

std::size_t g_allocations { 0 };
bool g_counting { false };

} // namespace

// The allocation test counts through a replacement global `operator new`, the
// same technique as src/kernel/tests/engine.test.cpp. ThreadSanitizer ships
// its own replacement, so the hook is unavailable there.
#if defined(__SANITIZE_THREAD__)
#  define CLOCKR_JAM_TEST_ALLOC_HOOK 0
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CLOCKR_JAM_TEST_ALLOC_HOOK 0
#  endif
#endif

#ifndef CLOCKR_JAM_TEST_ALLOC_HOOK
#  define CLOCKR_JAM_TEST_ALLOC_HOOK 1
#endif

#if CLOCKR_JAM_TEST_ALLOC_HOOK
namespace
{

void*
aligned_alloc_bytes(std::size_t bytes, std::size_t alignment) noexcept
{
#if defined(_MSC_VER)
    return _aligned_malloc(bytes, alignment);
#else
    return std::aligned_alloc(alignment, bytes);
#endif
}

void
aligned_free_bytes(void* p) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

void*
operator new(std::size_t n)
{
    if (g_counting)
        ++g_allocations;

    return std::malloc(n);
}

void*
operator new[](std::size_t n)
{
    if (g_counting)
        ++g_allocations;

    return std::malloc(n);
}

void*
operator new(std::size_t n, std::align_val_t align)
{
    if (g_counting)
        ++g_allocations;

    const auto a { static_cast<std::size_t>(align) };
    const auto rounded { ((n + a - 1) / a) * a };

    return aligned_alloc_bytes(rounded, a);
}

void*
operator new[](std::size_t n, std::align_val_t align)
{
    if (g_counting)
        ++g_allocations;

    const auto a { static_cast<std::size_t>(align) };
    const auto rounded { ((n + a - 1) / a) * a };

    return aligned_alloc_bytes(rounded, a);
}

void
operator delete(void* p) noexcept
{
    std::free(p);
}

void
operator delete[](void* p) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

void
operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::align_val_t) noexcept
{
    aligned_free_bytes(p);
}

void
operator delete[](void* p, std::align_val_t) noexcept
{
    aligned_free_bytes(p);
}

void
operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    aligned_free_bytes(p);
}

void
operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    aligned_free_bytes(p);
}
#endif

namespace
{

using namespace clockr;

void
set_ui(kernel::program& prog,
       bool playing = true,
       double bpm = 120.0,
       double loop_start = 0.0,
       double loop_end = 4.0,
       bool loop_on = true,
       double master = 0.8)
{
    prog.set("playing", playing);
    prog.set("bpm", bpm);
    prog.set<daw::LoopRegion>("loop", loop_start, loop_end, loop_on);
    prog.set("master", master);
}

struct readings
{
    double playhead {};
    jam::event_batch events {};
    jam::voice_state voices {};
    double mix {};
};

template<typename engine_t>
readings
run_block(kernel::program& prog,
          engine_t& eng,
          std::int64_t block_start,
          std::int64_t n_frames = kBeatBlock)
{
    prog.set<daw::BlockDesc>("block", block_start, n_frames, kRate);
    eng.step(prog, kernel::clock_tag::audio);

    return readings {
        kernel::get<double>(prog, "playhead"),
        kernel::get<jam::event_batch>(prog, "events"),
        kernel::get<jam::voice_state>(prog, "voices"),
        kernel::get<double>(prog, "mix"),
    };
}

void
expect_event(const jam::event_batch& batch,
             std::size_t index,
             std::int8_t note,
             std::uint8_t velocity,
             std::int16_t offset,
             std::uint8_t track,
             bool on)
{
    REQUIRE(index < batch.count);
    REQUIRE(batch.events[index].note == note);
    REQUIRE(batch.events[index].velocity == velocity);
    REQUIRE(batch.events[index].offset == offset);
    REQUIRE(batch.events[index].track == track);
    REQUIRE(batch.events[index].on == on);
}

std::size_t
active_voices(const jam::voice_state& state)
{
    std::size_t count { 0 };

    for (const auto& v : state.slots)
        if (v.active)
            ++count;

    return count;
}

const jam::voice*
voice_for_track(const jam::voice_state& state, std::uint8_t track)
{
    for (const auto& v : state.slots)
        if (v.active && v.track == track)
            return &v;

    return nullptr;
}

} // namespace

TEST_CASE("jam/network | Golden note and offset stream", "[jam]")
{
    const auto initial { jam_test::small_score() };
    kernel::program prog { jam::net::app(initial) };
    kernel::release_engine eng;

    jam::net::init(prog, eng, initial);
    set_ui(prog);
    eng.step(prog, kernel::clock_tag::ui);

    const auto b0 { run_block(prog, eng, 0) };
    const auto b1 { run_block(prog, eng, kBeatBlock) };
    const auto b2 { run_block(prog, eng, 2 * kBeatBlock) };
    const auto b3 { run_block(prog, eng, 3 * kBeatBlock) };
    const auto b4 { run_block(prog, eng, 4 * kBeatBlock) };

    REQUIRE(b0.playhead == Catch::Approx(1.0));
    REQUIRE(b1.playhead == Catch::Approx(2.0));
    REQUIRE(b2.playhead == Catch::Approx(3.0));
    REQUIRE(b3.playhead == Catch::Approx(0.0));
    REQUIRE(b4.playhead == Catch::Approx(1.0));

    for (const auto* b : { &b0, &b1, &b2, &b3, &b4 })
    {
        REQUIRE(b->events.count == 4);
        REQUIRE(b->events.dropped == 0);

        expect_event(b->events, 0, 60, 100, 0, 0, true);
        expect_event(b->events, 1, 60, 0, 6000, 0, false);
        expect_event(b->events, 2, 64, 80, 12000, 0, true);
        expect_event(b->events, 3, 64, 0, 18000, 0, false);
    }
}

TEST_CASE("jam/network | Voice state and mix follow the scheduled gates", "[jam]")
{
    const auto initial { jam_test::small_score() };
    kernel::program prog { jam::net::app(initial) };
    kernel::release_engine eng;

    jam::net::init(prog, eng, initial);
    set_ui(prog);
    eng.step(prog, kernel::clock_tag::ui);

    const auto t0 { run_block(prog, eng, 0, kStepBlock) };
    REQUIRE(t0.events.count == 1);
    expect_event(t0.events, 0, 60, 100, 0, 0, true);
    REQUIRE(active_voices(t0.voices) == 1);
    REQUIRE(t0.mix == Catch::Approx(0.8 * 100.0 / 127.0));

    const auto t1 { run_block(prog, eng, kStepBlock, kStepBlock) };
    REQUIRE(t1.events.count == 1);
    expect_event(t1.events, 0, 60, 0, 0, 0, false);
    REQUIRE(active_voices(t1.voices) == 0);
    REQUIRE(t1.mix == Catch::Approx(0.0));

    const auto t2 { run_block(prog, eng, 2 * kStepBlock, kStepBlock) };
    REQUIRE(t2.events.count == 1);
    expect_event(t2.events, 0, 64, 80, 0, 0, true);
    REQUIRE(active_voices(t2.voices) == 1);
    REQUIRE(t2.mix == Catch::Approx(0.8 * 80.0 / 127.0));

    const auto t3 { run_block(prog, eng, 3 * kStepBlock, kStepBlock) };
    REQUIRE(t3.events.count == 1);
    expect_event(t3.events, 0, 64, 0, 0, 0, false);
    REQUIRE(active_voices(t3.voices) == 0);
}

TEST_CASE("jam/network | A patch applies once at its epoch", "[jam]")
{
    const auto initial { jam_test::small_score() };
    kernel::program prog { jam::net::app(initial) };
    kernel::release_engine eng;

    jam::net::init(prog, eng, initial);
    set_ui(prog);
    eng.step(prog, kernel::clock_tag::ui);

    const auto baseline { run_block(prog, eng, 0) };
    expect_event(baseline.events, 0, 60, 100, 0, 0, true);

    // A transpose is not idempotent: a second application would shift twice.
    const auto patch { jam_test::make_patch({
        jam::patch_step { jam::patch_op::transpose, 0, 0, 0, 12 },
    }) };

    prog.set<jam::score_patch>("patch", patch);
    eng.step(prog, kernel::clock_tag::ui);

    const auto edited { run_block(prog, eng, kBeatBlock) };
    expect_event(edited.events, 0, 72, 100, 0, 0, true);

    // The next ui tick publishes a disengaged event; the score must hold.
    eng.step(prog, kernel::clock_tag::ui);

    const auto held { run_block(prog, eng, 2 * kBeatBlock) };
    expect_event(held.events, 0, 72, 100, 0, 0, true);
    expect_event(held.events, 2, 76, 80, 12000, 0, true);
}

TEST_CASE("jam/network | Same-epoch level set order does not matter", "[jam]")
{
    const auto initial { jam_test::small_score() };
    const auto patch { jam_test::make_patch({
        jam::patch_step { jam::patch_op::set_step, 0, 1, 62, 70 },
    }) };

    kernel::program a { jam::net::app(initial) };
    kernel::program b { jam::net::app(initial) };
    kernel::release_engine ea;
    kernel::release_engine eb;

    // Distinct ports, one epoch, opposite set order (patch included).
    a.set("playing", true);
    a.set("bpm", 132.0);
    a.set<daw::LoopRegion>("loop", 0.0, 4.0, true);
    a.set("master", 0.7);
    a.set<jam::score_patch>("patch", patch);

    b.set("master", 0.7);
    b.set<jam::score_patch>("patch", patch);
    b.set<daw::LoopRegion>("loop", 0.0, 4.0, true);
    b.set("bpm", 132.0);
    b.set("playing", true);

    ea.step(a, kernel::clock_tag::ui);
    eb.step(b, kernel::clock_tag::ui);

    for (std::int64_t k { 0 }; k < 3; ++k)
    {
        const auto ra { run_block(a, ea, k * kBeatBlock) };
        const auto rb { run_block(b, eb, k * kBeatBlock) };

        REQUIRE(rb.playhead == Catch::Approx(ra.playhead));
        REQUIRE(rb.events == ra.events);
        REQUIRE(rb.voices == ra.voices);
        REQUIRE(rb.mix == Catch::Approx(ra.mix));
    }
}

TEST_CASE("jam/network | Voice allocation steals the oldest", "[jam]")
{
    jam::event_batch batch {};

    for (std::uint8_t i { 0 }; i < 9; ++i)
        batch.events[i] = jam::note_event {
            .note = static_cast<std::int8_t>(60 + i),
            .velocity = 100,
            .offset = 0,
            .track = i,
            .on = true,
        };

    batch.count = 9;

    const auto state { jam::voice_tick(jam::voice_state {}, batch) };

    REQUIRE(active_voices(state) == jam::voice_count);
    REQUIRE(voice_for_track(state, 0) == nullptr);
    REQUIRE(voice_for_track(state, 8) != nullptr);
    REQUIRE(voice_for_track(state, 8)->note == 68);

    // An off releases the matching track and leaves the rest alone.
    jam::event_batch off {};
    off.events[0] = jam::note_event {
        .note = 65,
        .velocity = 0,
        .offset = 0,
        .track = 5,
        .on = false,
    };
    off.count = 1;

    const auto released { jam::voice_tick(state, off) };
    REQUIRE(voice_for_track(released, 5) == nullptr);
    REQUIRE(active_voices(released) == jam::voice_count - 1);

    // A silent-mask entry releases a track before events are applied.
    jam::event_batch muted {};
    muted.silent = static_cast<std::uint8_t>(1u << 1);

    const auto silenced { jam::voice_tick(state, muted) };
    REQUIRE(voice_for_track(silenced, 1) == nullptr);
}

TEST_CASE("jam/network | Event capacity is bounded and overflow is counted", "[jam]")
{
    auto s { jam_test::small_score() };

    s.patterns[0].steps[1] = jam::step { 62, 90, true };
    s.patterns[0].steps[3] = jam::step { 65, 90, true };

    for (std::size_t track_index { 0 }; track_index < jam::track_count; ++track_index)
        s.tracks[track_index] = jam::track {
            static_cast<std::uint8_t>(0),
            0,
            255,
            false,
        };

    // Two beats: eight 16th boundaries, 16 events each — more than capacity.
    const auto batch { jam::schedule(
        s,
        daw::PosPair { 0.0, 2.0 },
        daw::LoopRegion { 0.0, 4.0, true },
        daw::BlockDesc { 0, 48000, kRate },
        120.0) };

    REQUIRE(batch.count == jam::event_capacity);
    REQUIRE(batch.dropped > 0);

    // One beat is exactly the capacity for eight fully gated tracks.
    const auto fits { jam::schedule(
        s,
        daw::PosPair { 0.0, 1.0 },
        daw::LoopRegion { 0.0, 4.0, true },
        daw::BlockDesc { 0, 24000, kRate },
        120.0) };

    REQUIRE(fits.count == jam::event_capacity);
    REQUIRE(fits.dropped == 0);
}

TEST_CASE("jam/network | Programs register the jam debug schemas", "[jam]")
{
    // The musical data is plain, trivially-copyable value data; large values
    // ride pool cells automatically.
    static_assert(std::is_trivially_copyable_v<jam::step>);
    static_assert(std::is_trivially_copyable_v<jam::pattern>);
    static_assert(std::is_trivially_copyable_v<jam::track>);
    static_assert(std::is_trivially_copyable_v<jam::score>);
    static_assert(std::is_trivially_copyable_v<jam::patch_step>);
    static_assert(std::is_trivially_copyable_v<jam::score_patch>);
    static_assert(std::is_trivially_copyable_v<jam::note_event>);
    static_assert(std::is_trivially_copyable_v<jam::event_batch>);
    static_assert(std::is_trivially_copyable_v<jam::voice>);
    static_assert(std::is_trivially_copyable_v<jam::voice_state>);
    static_assert(sizeof(jam::score) > clockr::kernel::user_inline_capacity);

    kernel::program prog { jam::net::app(jam_test::small_score()) };

    const auto find = [&](std::string_view name)
    {
        for (const auto* type : prog.types())
            if (type->name != nullptr && std::string_view { type->name } == name)
                return true;

        return false;
    };

    // Directly named by the network.
    REQUIRE(find("jam.score"));
    REQUIRE(find("jam.score_patch"));
    REQUIRE(find("jam.event_batch"));
    REQUIRE(find("jam.voice_state"));

    // Every wire struct carries a schema, whether or not this particular
    // graph names it directly.
    using clockr::kernel::describe;

    REQUIRE(std::string_view { describe<jam::step>::type().name } == "jam.step");
    REQUIRE(std::string_view { describe<jam::pattern>::type().name } == "jam.pattern");
    REQUIRE(std::string_view { describe<jam::track>::type().name } == "jam.track");
    REQUIRE(std::string_view { describe<jam::score>::type().name } == "jam.score");
    REQUIRE(std::string_view { describe<jam::patch_step>::type().name } == "jam.patch_step");
    REQUIRE(std::string_view { describe<jam::score_patch>::type().name } == "jam.score_patch");
    REQUIRE(std::string_view { describe<jam::note_event>::type().name } == "jam.note_event");
    REQUIRE(std::string_view { describe<jam::event_batch>::type().name } == "jam.event_batch");
    REQUIRE(std::string_view { describe<jam::voice>::type().name } == "jam.voice");
    REQUIRE(std::string_view { describe<jam::voice_state>::type().name } == "jam.voice_state");
}

TEST_CASE("jam/network | Owning and borrowing programs agree", "[jam]")
{
    const auto initial { jam_test::small_score() };
    const auto recipe { jam::net::app(initial) };

    kernel::program owned { recipe };

    const auto net { clockr::shell::compile(recipe) };
    kernel::program borrowed { net.descriptor() };

    kernel::release_engine eo;
    kernel::release_engine eb;

    jam::net::init(owned, eo, initial);
    jam::net::init(borrowed, eb, initial);

    set_ui(owned);
    set_ui(borrowed);
    eo.step(owned, kernel::clock_tag::ui);
    eb.step(borrowed, kernel::clock_tag::ui);

    const auto patch { jam_test::make_patch({
        jam::patch_step { jam::patch_op::set_step, 0, 1, 62, 70 },
    }) };

    for (std::int64_t k { 0 }; k < 3; ++k)
    {
        if (k == 1)
        {
            owned.set<jam::score_patch>("patch", patch);
            borrowed.set<jam::score_patch>("patch", patch);
            eo.step(owned, kernel::clock_tag::ui);
            eb.step(borrowed, kernel::clock_tag::ui);
        }

        const auto ro { run_block(owned, eo, k * kBeatBlock) };
        const auto rb { run_block(borrowed, eb, k * kBeatBlock) };

        REQUIRE(rb.playhead == Catch::Approx(ro.playhead));
        REQUIRE(rb.events == ro.events);
        REQUIRE(rb.voices == ro.voices);
        REQUIRE(rb.mix == Catch::Approx(ro.mix));
    }
}

TEST_CASE("jam/network | Debug and release engines agree on a patch script", "[jam]")
{
    const auto initial { jam_test::small_score() };

    kernel::program release_prog { jam::net::app(initial) };
    kernel::program debug_prog { jam::net::app(initial) };

    std::string trace;
    kernel::text_trace_sink sink { trace };
    kernel::debug_engine debug { sink };
    kernel::release_engine release;

    jam::net::init(release_prog, release, initial);
    jam::net::init(debug_prog, debug, initial);

    set_ui(release_prog);
    set_ui(debug_prog);
    release.step(release_prog, kernel::clock_tag::ui);
    debug.step(debug_prog, kernel::clock_tag::ui);

    const auto patch { jam_test::make_patch({
        jam::patch_step { jam::patch_op::transpose, 0, 0, 0, 7 },
    }) };

    for (std::int64_t k { 0 }; k < 5; ++k)
    {
        if (k == 2)
        {
            release_prog.set<jam::score_patch>("patch", patch);
            debug_prog.set<jam::score_patch>("patch", patch);
            release.step(release_prog, kernel::clock_tag::ui);
            debug.step(debug_prog, kernel::clock_tag::ui);
        }

        const auto r { run_block(release_prog, release, k * kBeatBlock) };
        const auto d { run_block(debug_prog, debug, k * kBeatBlock) };

        REQUIRE(d.playhead == Catch::Approx(r.playhead));
        REQUIRE(d.events == r.events);
        REQUIRE(d.voices == r.voices);
        REQUIRE(d.mix == Catch::Approx(r.mix));
    }
}

TEST_CASE("jam/network | Evaluate performs no heap allocations", "[jam]")
{
#if !CLOCKR_JAM_TEST_ALLOC_HOOK
    SKIP("allocation counting is unavailable under ThreadSanitizer");
#endif

    const auto initial { jam_test::small_score() };
    kernel::program prog { jam::net::app(initial) };
    kernel::release_engine eng;

    jam::net::init(prog, eng, initial);
    set_ui(prog);
    eng.step(prog, kernel::clock_tag::ui);

    // Warm the queues and cross-region paths for one full tick.
    run_block(prog, eng, 0);

    const auto patch { jam_test::make_patch({
        jam::patch_step { jam::patch_op::transpose, 0, 0, 0, 12 },
    }) };

    prog.set<jam::score_patch>("patch", patch);
    prog.set<daw::BlockDesc>("block", kBeatBlock, kBeatBlock, kRate);

    g_counting = true;
    g_allocations = 0;

    eng.step(prog, kernel::clock_tag::ui);
    eng.step(prog, kernel::clock_tag::audio);

    g_counting = false;

    REQUIRE(g_allocations == 0);
    REQUIRE(kernel::get<jam::event_batch>(prog, "events").count == 4);
}
