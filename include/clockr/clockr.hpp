#pragma once

/// @file clockr.hpp
/// Umbrella header for normal use: the typed shell (`clockr/shell/...`)
/// plus the program and engine that execute it. Include a module header
/// directly when only one layer is needed (`clockr::mem`, `clockr::conc`,
/// `clockr::kernel` or `clockr::shell`).

#include "clockr/kernel/engine.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/kernel/debug_engine.hpp"
#include "clockr/shell/recipe.hpp"
