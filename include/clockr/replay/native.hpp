#pragma once

/// @file
/// Native recipe-backed replay factory.

#include "clockr/replay/factory.hpp"

#include "clockr/kernel/program.hpp"

#include <memory>
#include <utility>

namespace clockr::replay::native
{

/// A `program_factory` that captures a shell recipe by value and constructs an
/// owning `kernel::program` for each replay.
template<typename Recipe>
class recipe_factory final : public program_factory
{
public:
    explicit recipe_factory(Recipe recipe) :
        m_recipe { std::move(recipe) }
    {}

    std::unique_ptr<kernel::program>
    make(const scenario&, kernel::program_options options) override
    {
        return std::make_unique<kernel::program>(m_recipe, options);
    }

private:
    Recipe m_recipe;
};

template<typename Recipe>
recipe_factory(Recipe) -> recipe_factory<Recipe>;

} // namespace clockr::replay::native
