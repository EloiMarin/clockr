#pragma once

// synth.hpp — the dumb player.
//
// P1 produces scheduling facts (an on count, a sample offset, a per-block
// voice gain), not audio. This is the smallest useful thing that turns those
// facts into sound: a fixed-note sine with a short envelope, started at the
// reported sample offset and released on the off crossing. It is deliberately
// outside the engine, like every other piece of sample copying in the demo.
//
// Only the audio callback touches a synth_state, so it is plain data with no
// locks or allocation.

#include "types.hpp"

#include <cstdint>

namespace daw::native
{

struct synth_state
{
    double phase { 0.0 };
    double frequency { 0.0 };
    double envelope { 0.0 };
    double gain { 0.0 };
    bool active { false };
    bool releasing { false };
};

/// Render one block. `output` is interleaved f32 with `channels` channels and
/// must have room for `frames * channels` samples. The engine's block outputs
/// decide when the note starts (sample offset) and when it is released.
void
render_synth(
    synth_state& state,
    const audio_outputs& outputs,
    float* output,
    std::uint32_t frames,
    double sample_rate,
    std::uint32_t channels
);

/// Forget the note (used when the audio clock is paused or a tick throws).
void
reset_synth(synth_state& state);

} // namespace daw::native
