#include "clockr/replay/compare.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace clockr::replay
{

namespace
{

bool
trace_equal(const kernel::trace_event& lhs, const kernel::trace_event& rhs)
{
    return lhs.kind == rhs.kind && lhs.clock == rhs.clock &&
           lhs.node_id == rhs.node_id && lhs.payload == rhs.payload;
}

} // namespace

run_diff
compare(const run& expected, const run& actual, compare_options)
{
    run_diff diff;

    const auto common_steps {
        std::min(expected.steps.size(), actual.steps.size())
    };

    for (std::size_t i { 0 }; i < common_steps; ++i)
    {
        const auto& expected_step { expected.steps[i] };
        const auto& actual_step { actual.steps[i] };
        bool diverged { false };

        if (expected_step.step != actual_step.step ||
            expected_step.threw != actual_step.threw)
        {
            diverged = true;
            ++diff.output_mismatches;
        }

        const auto common_outputs {
            std::min(expected_step.outputs.size(), actual_step.outputs.size())
        };

        for (std::size_t port { 0 }; port < common_outputs; ++port)
        {
            if (encoded_value_equal(
                    expected_step.outputs[port], actual_step.outputs[port]
                ))
                continue;

            diff.values.push_back(
                value_mismatch { i,
                                 port,
                                 expected_step.outputs[port],
                                 actual_step.outputs[port] }
            );
            ++diff.output_mismatches;
            diverged = true;
        }

        if (expected_step.outputs.size() != actual_step.outputs.size())
        {
            const auto larger { std::max(
                expected_step.outputs.size(), actual_step.outputs.size()
            ) };
            diff.output_mismatches += larger - common_outputs;
            diverged = true;
        }

        if (diverged)
        {
            if (diff.equal)
                diff.first_step = i;

            diff.equal = false;
        }
    }

    if (expected.steps.size() != actual.steps.size())
    {
        const auto larger {
            std::max(expected.steps.size(), actual.steps.size())
        };
        diff.output_mismatches += larger - common_steps;

        if (diff.equal)
            diff.first_step = common_steps;

        diff.equal = false;
    }

    const auto common_trace {
        std::min(expected.trace.size(), actual.trace.size())
    };

    for (std::size_t i { 0 }; i < common_trace; ++i)
        if (!trace_equal(expected.trace[i], actual.trace[i]))
        {
            ++diff.trace_mismatches;
            diff.equal = false;
        }

    if (expected.trace.size() != actual.trace.size())
    {
        const auto larger {
            std::max(expected.trace.size(), actual.trace.size())
        };
        diff.trace_mismatches += larger - common_trace;
        diff.equal = false;
    }

    if (diff.equal)
    {
        diff.summary = "runs are semantically equal";
    }
    else
    {
        diff.summary =
            "runs differ: " + std::to_string(diff.output_mismatches) +
            " output mismatch(es), " + std::to_string(diff.trace_mismatches) +
            " trace mismatch(es), steps " +
            std::to_string(expected.steps.size()) + " vs " +
            std::to_string(actual.steps.size());
    }

    return diff;
}

} // namespace clockr::replay
