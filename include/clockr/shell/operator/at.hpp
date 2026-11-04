#pragma once

// at: "the value of this network, as seen on that clock, with this resampling policy"
//     (doc/reference/denotational-semantics.md §3.2).
//     Used at the start of a chain, not composed after its source. Downstream of
//     at<> runs on the TARGET clock: the operator both annotates the
//     crossing and re-anchors the walker's clock for the rest of the chain.
#include "clockr/shell/node.hpp"
#include "clockr/shell/recipe.hpp"

namespace clockr::shell
{

/// Change clocks: view `network` on `tag_t`, resampling the crossing with
/// `policy_t` (`latest` or `queued`). The annotation re-anchors everything
/// downstream on the target clock, so `at` must head the fragment it crosses,
/// not follow its source.
template<
    kernel::clock_tag tag_t,
    conc::policy policy_t,
    typename in_t,
    typename out_t,
    typename... ast_tags_t,
    std::size_t tags_size
>
constexpr
auto
at(const recipe<in_t, out_t, skeleton::tag_list<ast_tags_t...>, tags_size>& network)
{
    return recipe<in_t, out_t, skeleton::tag_list<ast_tags_t...>, tags_size + 1>
    {
        detail::append_node(network.storage(), node { at_t{ network.root_index(), tag_t, policy_t } }),
        tags_size
    };
}

} // namespace clockr::shell
