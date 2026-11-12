#pragma once

/// @file
/// Semantic comparison of two replay runs.

#include "clockr/replay/run.hpp"
#include "clockr/replay/value_codec.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace clockr::replay
{

class codec_registry;

/// One differing output value.
struct value_mismatch
{
    std::size_t step { 0 };
    std::size_t port { 0 };
    encoded_value expected;
    encoded_value actual;
};

/// The semantic difference between two runs.
struct run_diff
{
    bool equal { true };
    std::size_t first_step { 0 };
    std::size_t output_mismatches { 0 };
    std::size_t trace_mismatches { 0 };
    std::vector<value_mismatch> values;
    std::string summary;
};

/// Comparison knobs. Codecs are available for callers that want value-aware
/// diagnostics; encoded user bytes compare semantically without them.
struct compare_options
{
    const codec_registry* codecs { nullptr };
};

/// Compare two runs by `(step, port/output, trace node)`, ignoring durations,
/// sequences, addresses and `user_cell*` identity.
run_diff
compare(const run& expected, const run& actual, compare_options options = {});

} // namespace clockr::replay
