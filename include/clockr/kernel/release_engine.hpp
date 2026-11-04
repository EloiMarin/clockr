#pragma once

#include "clockr/kernel/engine.hpp"

namespace clockr::kernel
{

/// The lean evaluator: no trace hooks, no timers. It must produce exactly the
/// same outputs as `debug_engine` for the same program (law L5, pinned by the
/// parity tests).
class release_engine final : public engine
{
public:
private:
};

} // namespace clockr::kernel
