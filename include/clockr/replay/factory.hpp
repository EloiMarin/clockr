#pragma once

/// @file
/// Host-supplied fresh-program construction for replay.

#include "clockr/replay/scenario.hpp"

#include "clockr/kernel/program.hpp"

#include <memory>

namespace clockr::replay
{

/// Builds the program a scenario is replayed into. The factory owns the host's
/// recipe or descriptor; replay only asks for a fresh program per run.
class program_factory
{
public:
    virtual ~program_factory() = default;

    virtual std::unique_ptr<kernel::program>
    make(const scenario&, kernel::program_options options = {}) = 0;
};

} // namespace clockr::replay
