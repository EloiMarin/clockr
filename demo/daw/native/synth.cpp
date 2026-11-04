#include "synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace daw::native
{

namespace
{
// The scripted P1 note is MIDI 60. P1's outputs do not carry the voice id
// yet, so the dumb player is deliberately a one-note instrument.
constexpr double kMidi60Hz { 261.6255653005986 };
constexpr double kAmplitude { 0.25 };
constexpr double kAttackSeconds { 0.005 };
constexpr double kReleaseSeconds { 0.020 };
} // namespace

void
render_synth(
    synth_state& state,
    const audio_outputs& outputs,
    float* output,
    std::uint32_t frames,
    double sample_rate,
    std::uint32_t channels
)
{
    const auto frame_count { static_cast<std::size_t>(frames) };
    const auto channel_count { static_cast<std::size_t>(channels) };

    std::size_t start_at { 0 };

    if (outputs.midi_count > 0)
    {
        state.active = true;
        state.releasing = false;
        state.envelope = 0.0;
        state.phase = 0.0;
        state.frequency = kMidi60Hz;
        state.gain = outputs.voice_gain;

        if (outputs.midi_offset > 0)
            start_at = std::min(
                static_cast<std::size_t>(outputs.midi_offset), frame_count
            );
    }
    else if (state.active && outputs.midi_off_count == 0)
    {
        // Sustain follows the live master gain. The release ramp keeps the
        // gain it had when the off crossing arrived, so the note fades
        // instead of clicking shut with the voice gate.
        if (!state.releasing)
            state.gain = outputs.voice_gain;
    }

    if (outputs.midi_off_count > 0 && state.active && !state.releasing)
        state.releasing = true;

    const double rate { sample_rate > 0.0 ? sample_rate : 48000.0 };
    const double phase_step { (2.0 * std::numbers::pi * state.frequency) /
                              rate };
    const double attack_step { 1.0 / (kAttackSeconds * rate) };
    const double release_step { 1.0 / (kReleaseSeconds * rate) };

    for (std::size_t frame { 0 }; frame < frame_count; ++frame)
    {
        float sample { 0.0F };

        if (state.active && frame >= start_at)
        {
            if (state.releasing)
            {
                state.envelope -= release_step;

                if (state.envelope <= 0.0)
                {
                    state.envelope = 0.0;
                    state.active = false;
                }
            }
            else if (state.envelope < 1.0)
            {
                state.envelope = std::min(1.0, state.envelope + attack_step);
            }

            if (state.active)
            {
                sample = static_cast<float>(
                    std::sin(state.phase) * state.envelope * state.gain *
                    kAmplitude
                );

                state.phase += phase_step;

                if (state.phase >= 2.0 * std::numbers::pi)
                    state.phase -= 2.0 * std::numbers::pi;
            }
        }

        for (std::size_t channel { 0 }; channel < channel_count; ++channel)
            output[(frame * channel_count) + channel] = sample;
    }
}

void
reset_synth(synth_state& state)
{
    state = synth_state {};
}

} // namespace daw::native
