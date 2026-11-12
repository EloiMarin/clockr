#include "conductor.hpp"

#include "clockr/replay/compare.hpp"
#include "clockr/replay/native.hpp"
#include "clockr/replay/replayer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace
{

std::size_t g_allocations { 0 };
bool g_counting { false };

jam::score
demo_score()
{
    jam::score result {};

    auto& pattern { result.patterns[0] };
    pattern.length = 8;
    pattern.steps[0] = jam::step { 60, 100, true };
    pattern.steps[2] = jam::step { 64, 90, true };
    pattern.steps[4] = jam::step { 67, 80, true };
    pattern.steps[6] = jam::step { 72, 110, true };

    result.tracks[0] = jam::track { 0, 0, 220, false };
    result.tracks[1] = jam::track { 0, 12, 140, false };
    result.bpm = 120;

    return result;
}

jam::score_patch
transpose_patch(std::int16_t semitones)
{
    jam::score_patch patch {};
    patch.count = 1;
    patch.ops[0] = jam::patch_step {
        jam::patch_op::transpose, 0, 0, 0, semitones
    };

    return patch;
}

std::size_t
count_patch_entries(const clockr::replay::scenario& journal)
{
    std::size_t count { 0 };

    for (const auto& entry : journal.entries)
        if (entry.kind == clockr::replay::entry_kind::set_input &&
            entry.port == "patch")
            ++count;

    return count;
}

std::size_t
count_edit_marks(const clockr::replay::scenario& journal)
{
    std::size_t count { 0 };

    for (const auto& entry : journal.entries)
        if (entry.kind == clockr::replay::entry_kind::mark &&
            entry.note.find("participant=") != std::string::npos)
            ++count;

    return count;
}

int
run_selftest()
{
    bool ok { true };

    const auto expect = [&ok](bool condition, const char* name) {
        if (condition)
            std::printf("ok   %s\n", name);
        else
        {
            std::printf("FAIL %s\n", name);
            ok = false;
        }
    };

    const auto initial { demo_score() };

    {
        jam::native::conductor first { initial };
        jam::native::conductor second { initial };

        for (std::size_t i { 0 }; i < 24; ++i)
        {
            first.step_block();
            second.step_block();
        }

        expect(
            first.digest() == second.digest(),
            "deterministic schedule: two conductors share a digest"
        );
        expect(
            first.live_run().steps.size() == second.live_run().steps.size(),
            "deterministic schedule: step counts match"
        );
    }

    {
        jam::native::conductor live { initial };

        jam::native::participant_edit edit;
        edit.participant = 7;
        edit.revision = 1;
        edit.patch = transpose_patch(12);

        expect(live.submit(edit), "scripted edit accepted on the queue");

        for (std::size_t i { 0 }; i < 24; ++i)
            live.step_block();

        const auto& journal { live.finish_recording() };

        expect(journal.fidelity.complete, "journal is complete");
        expect(count_patch_entries(journal) == 1, "journal records the patch");

        clockr::replay::native::recipe_factory factory { live.recipe() };
        clockr::replay::replayer replayer;
        clockr::replay::replay_options options;
        options.capture_trace = false;
        options.program_options.region_pool_bytes =
            live.options().region_pool_bytes;

        const auto replayed {
            replayer.replay(journal, factory, live.codecs(), options)
        };
        const auto diff {
            clockr::replay::compare(live.live_run(), replayed)
        };

        expect(replayed.fidelity.complete, "replay is complete");
        expect(diff.equal, "replayed journal equals the live run");
        expect(
            live.digest() == jam::native::run_digest(replayed),
            "replayed digest equals the live digest"
        );
    }

    {
        jam::native::conductor queued { initial };

        jam::native::participant_edit edit;
        edit.participant = 3;
        edit.revision = 5;
        edit.patch = transpose_patch(5);

        expect(queued.submit(edit), "edit enters the SPSC queue");
        expect(queued.pending_edits() == 1, "queue reports one pending edit");

        queued.advance(8);
        expect(
            queued.edits_applied() == 0,
            "edit waits for a conductor boundary"
        );

        queued.advance(1);
        expect(
            queued.edits_applied() == 1,
            "edit is applied at the ui boundary"
        );
        expect(queued.pending_edits() == 0, "queue is drained after the edit");
        expect(
            count_patch_entries(queued.recorded()) == 1,
            "recording contains the applied patch"
        );
        expect(
            count_edit_marks(queued.recorded()) == 1,
            "recording contains the participant/revision mark"
        );

        jam::native::conductor_options small;
        small.edit_capacity = 4;
        jam::native::conductor full { initial, small };

        bool accepted { true };

        for (std::size_t i { 0 }; i < small.edit_capacity; ++i)
            if (!full.submit(edit))
                accepted = false;

        expect(accepted, "edits fill the bounded queue");
        expect(
            !full.submit(edit),
            "a full edit queue reports instead of blocking"
        );
        expect(full.edits_dropped() == 1, "dropped edit is counted");
    }

    {
        jam::native::conductor ring { initial };

        constexpr std::size_t block_count { 64 };
        ring.advance(block_count);

        expect(
            ring.blocks_rendered() == block_count,
            "every rendered block enters the ring"
        );
        expect(ring.blocks_dropped() == 0, "ring never drops in this run");

        std::size_t drained { 0 };
        double mix_total { 0.0 };
        jam::native::rendered_block block;

        g_allocations = 0;
        g_counting = true;

        while (ring.pop_block(block))
        {
            ++drained;
            mix_total += block.mix;
        }

        g_counting = false;

        expect(drained == block_count, "consumer drains every block");
        expect(mix_total >= 0.0, "drained mix values are bounded");
        expect(g_allocations == 0, "callback path performs no heap allocation");
    }

    if (ok)
        std::printf("jam_conductor selftest: all checks passed\n");
    else
        std::printf("jam_conductor selftest: FAILED\n");

    return ok ? 0 : 1;
}

void
print_help()
{
    std::printf(
        "jam_conductor - deterministic headless conductor for the jam network\n"
        "\n"
        "usage: jam_conductor [--selftest] [--help]\n"
        "\n"
        "  --selftest  run the conductor/journal/ring acceptance checks\n"
        "  --help      show this text\n"
    );
}

} // namespace

// The allocation checks below count through a replacement global `operator
// new`. ThreadSanitizer ships its own replacement, so the hook is unavailable
// there; the counting flag then stays inert (the check runs vacuously).
#if defined(__SANITIZE_THREAD__)
#  define CLOCKR_JAM_ALLOC_HOOK 0
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CLOCKR_JAM_ALLOC_HOOK 0
#  endif
#endif

#ifndef CLOCKR_JAM_ALLOC_HOOK
#  define CLOCKR_JAM_ALLOC_HOOK 1
#endif

#if CLOCKR_JAM_ALLOC_HOOK
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
aligned_free_bytes(void* pointer) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

} // namespace

void*
operator new(std::size_t count)
{
    if (g_counting)
        ++g_allocations;

    return std::malloc(count);
}

void*
operator new[](std::size_t count)
{
    if (g_counting)
        ++g_allocations;

    return std::malloc(count);
}

void*
operator new(std::size_t count, std::align_val_t alignment)
{
    if (g_counting)
        ++g_allocations;

    const auto align { static_cast<std::size_t>(alignment) };
    const auto rounded { ((count + align - 1) / align) * align };

    return aligned_alloc_bytes(rounded, align);
}

void*
operator new[](std::size_t count, std::align_val_t alignment)
{
    if (g_counting)
        ++g_allocations;

    const auto align { static_cast<std::size_t>(alignment) };
    const auto rounded { ((count + align - 1) / align) * align };

    return aligned_alloc_bytes(rounded, align);
}

void
operator delete(void* pointer) noexcept
{
    std::free(pointer);
}

void
operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}

void
operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void
operator delete[](void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void
operator delete(void* pointer, std::align_val_t) noexcept
{
    aligned_free_bytes(pointer);
}

void
operator delete[](void* pointer, std::align_val_t) noexcept
{
    aligned_free_bytes(pointer);
}

void
operator delete(void* pointer, std::size_t, std::align_val_t) noexcept
{
    aligned_free_bytes(pointer);
}

void
operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept
{
    aligned_free_bytes(pointer);
}
#endif // CLOCKR_JAM_ALLOC_HOOK

int
main(int argc, char** argv)
{
    if (argc > 1)
    {
        const std::string_view argument { argv[1] };

        if (argument == "--selftest")
            return run_selftest();

        if (argument == "--help" || argument == "-h")
        {
            print_help();
            return 0;
        }

        std::printf("unknown argument: %s\n", argv[1]);
        print_help();
        return 2;
    }

    print_help();
    return 2;
}
