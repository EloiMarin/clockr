#pragma once

#include <cstdint>

namespace clockr::kernel
{

/// A human-readable identity for a lowered node, so tools (a graph view,
/// DOT export, diagnostics) can name what a node *is* rather than show a bare
/// id.
///
/// `combinator` is a static string ("input", "output", "lift", "scan",
/// "delay"); `port_name` names the boundary port for input/output nodes and is
/// null elsewhere. Purely descriptive: evaluation never reads it, so a null
/// label is legal (hand-written descriptors may omit labels entirely).
///
/// The field is spelled `combinator` — the code's alias for what docs call an
/// operator, because the C++ keyword `operator` cannot be an identifier (see
/// GLOSSARY.md, Operators).
struct node_label
{
    const char* combinator { nullptr };
    const char* port_name { nullptr };
};

} // namespace clockr::kernel
