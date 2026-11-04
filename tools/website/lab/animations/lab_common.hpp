#pragma once

#include "clockr/kernel/program.hpp"

#include <cstdint>
#include <limits>
#include <variant>

namespace lab
{
/// Read a named output as a double. A not-yet-produced value (or a non-number)
/// reads as NaN, matching the native demo host's tolerant getters.
inline double
read_double(const clockr::kernel::program& program, const char* name)
{
    const clockr::kernel::value value { program.get(name) };

    if (const auto* d { std::get_if<double>(&value) })
        return *d;

    if (const auto* i { std::get_if<std::int64_t>(&value) })
        return static_cast<double>(*i);

    return std::numeric_limits<double>::quiet_NaN();
}

inline bool
read_bool(const clockr::kernel::program& program, const char* name)
{
    const clockr::kernel::value value { program.get(name) };

    if (const auto* b { std::get_if<bool>(&value) })
        return *b;

    return false;
}
} // namespace lab
