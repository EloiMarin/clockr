#pragma once

// share: Fan-out. It can be the input to other multiple recipe, but it's only
// instantiated once and evaluated once per tick.

#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

#include <cstdint>
#include <source_location>
#include <utility>

namespace clockr::shell
{

// The share identity is *nominal*: "the same `share()` call used twice dedups;
// two distinct `share()` calls don't." In a by-value arena that identity must
// be carried as a value, so it survives the deep copies `operator>>`/`zip`/
// `parallel` perform. It is minted once per source-level `share(...)` token
// from that token's `std::source_location` (line:column — a compile-time
// constant, unique across the call sites of a single network's composition),
// packed into `share_t::m_id`. No runtime counter: the id is a pure function
// of the source, so `share` stays constexpr and deterministic, and the whole
// operator set remains compile-able (§G: any program the library can
// express can be `shell::compile`'d).
//
// The nominal id has one sharp edge: a *helper function* that shares a
// fragment is one call site, so every call to the helper shares the same id.
// That is what makes `auto in() { return share(input<double, ui>("x")); }`
// called twice dedup correctly — but a helper *parameterized by port name*
// (`auto in(name) { return share(input<double, ui>(name)); }`) would mint one
// id for two distinct ports and silently collapse them onto the first. The
// walk rejects that case at build time (see share_t::instantiate); give each
// parameter its own `share()` call site instead.
/// Fan-out: a shared fragment is instantiated once and evaluated once per
/// tick, however many consumers it has. Identity is nominal — minted from the
/// `share(...)` call site's source location — so the same call site used
/// twice dedups, while distinct call sites do not.
template<typename in_t, typename out_t, typename... Tags, std::size_t N>
constexpr
recipe<in_t, out_t, skeleton::tag_list<Tags...>, N + 1>
share(
    const recipe<in_t, out_t, skeleton::tag_list<Tags...>, N>& network,
    std::source_location loc = std::source_location::current()
)
{
    const std::uint64_t id { (static_cast<std::uint64_t>(loc.line()) << 32u) | static_cast<std::uint64_t>(loc.column()) };

    auto arena { detail::append_node(network.storage(), node { share_t { network.root_index(), id } }) };

    return recipe<in_t, out_t, skeleton::tag_list<Tags...>, N + 1>
    {
        std::move(arena),
        N
    };
}

} // namespace clockr::shell
