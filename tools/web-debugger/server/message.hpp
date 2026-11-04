#pragma once

// The debug wire protocol: protobuf messages defined in
// proto/clockr_debug.proto, plus the conversions between them and the
// library's own types. All generated types live in namespace
// clockr::debug::v1 and are aliased here as `pb` to keep the call sites
// readable.

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/value.hpp"
#include "clockr/kernel/node.hpp"

#include "clockr/kernel/trace.hpp"

#include "clockr/conc/link.hpp"

#include <clockr_debug.pb.h>

#include <cstdint>
#include <string>

namespace clockr::debug
{

namespace pb = ::clockr::debug::v1;

// --- value --------------------------------------------------------------

/// kernel::value -> proto Value. monostate becomes the explicit `unit`
/// alternative so "monostate" and "absent payload" stay distinguishable.
pb::Value
to_message(const kernel::value&);

/// proto Value -> kernel::value. An unset oneof reads back as monostate.
kernel::value
from_message(const pb::Value&);

// --- thread / kind names ------------------------------------------------

std::string
to_message(kernel::clock_tag);

std::string
to_message(kernel::node_kind);

/// The static combinator label of a node, or empty when the descriptor
/// carries none (see kernel::label).
std::string
node_label_text(const kernel::descriptor_view&, kernel::node_id);

std::string
node_port_text(const kernel::descriptor_view&, kernel::node_id);

// --- graph description --------------------------------------------------

/// Encode the static structure of `program` (nodes, positional fan-in,
/// regions' thread tags, boundary port names, crossings). Sent once to
/// a connecting client, before any events.
pb::GraphDescription
describe(const kernel::program&);

// --- events -------------------------------------------------------------

/// Encode one recorded event. `stamp_ns` is the producer-thread capture time
/// supplied by the recorder (the engine's trace_event carries no clock).
pb::TraceEvent
to_message(const kernel::trace_event& event, std::uint64_t stamp_ns);

/// Decode one event. Throws std::runtime_error on an unknown kind.
kernel::trace_event
from_message(const pb::TraceEvent&);

} // namespace clockr::debug
