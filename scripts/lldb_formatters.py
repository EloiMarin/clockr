#!/usr/bin/env python3
"""LLDB type summaries for clockr's runtime value types.

Makes `kernel::value` (a `std::variant<monostate, bool, int64_t, double>`)
readable in the debugger: instead of libstdc++'s nested `_M_u`/`_M_index`
machinery you see `int64_t<3>`, `bool<true>`, `unit`, `double<2.5>`.

Types covered:
  - clockr::kernel::value
    (also registered under its canonical libstdc++ name, since lldb resolves
    the typedef and shows `std::variant<std::monostate, bool, long, double>`)
  - clockr::kernel::delay_state

Usage:
  CLI:      lldb -o 'command script import scripts/lldb_formatters.py' _build/linux_x64_debug/.../engine_tests
  Session:  command script import <repo>/scripts/lldb_formatters.py
  lldb-dap: "initCommands": ["command script import <repo>/scripts/lldb_formatters.py"]

The file is self-registering: lldb calls __lldb_init_module() on import.
"""

import lldb

VARIANT_TYPE_NAME = "std::variant<std::monostate, bool, long, double>"
VARIANT_TYPE_PREFIX = "std::variant<std::monostate, bool, long, double"


def _child(valobj, name):
    """Return named direct child, or None if absent/invalid."""
    if valobj is None or not valobj.IsValid():
        return None

    child = valobj.GetChildMemberWithName(name)
    return child if child.IsValid() else None


def _raw(valobj):
    """Return the non-synthetic value: synthetic (offset-derived) children of
    std::vector/container formatters misresolve fields of anonymous-union
    members like _M_index; lldb's own formatters read the raw object."""
    raw = valobj.GetNonSyntheticValue()
    return raw if raw.IsValid() else valobj


def _descend(valobj, name, depth=0):
    """Recursively search child types (base-class chains) for a named member."""
    if valobj is None or not valobj.IsValid() or depth > 8:
        return None

    child = _child(valobj, name)
    if child is not None:
        return child

    for i in range(valobj.GetNumChildren()):
        found = _descend(valobj.GetChildAtIndex(i), name, depth + 1)
        if found is not None:
            return found
    return None


def _find_variant(valobj, depth=0):
    """Find the first descendant whose canonical type is kernel::value's variant."""
    if valobj is None or not valobj.IsValid() or depth > 8:
        return None

    type_name = valobj.GetType().GetCanonicalType().GetName()
    if type_name and type_name.startswith(VARIANT_TYPE_PREFIX):
        return _raw(valobj)

    for i in range(valobj.GetNumChildren()):
        found = _find_variant(valobj.GetChildAtIndex(i), depth + 1)
        if found is not None:
            return found
    return None


def _optional_payload(valobj):
    """Extract (engaged, payload variant) from a libstdc++ std::optional.

    Works both on the raw layout (payload hidden behind _Optional_base) and
    on the built-in synthetic view, where the payload surfaces as a "Value"
    child and _M_engaged is not reachable.
    """
    raw = _raw(valobj)
    engaged = _descend(raw, "_M_engaged")
    if engaged is not None:
        is_engaged = engaged.GetValueAsUnsigned(0) != 0
    else:
        is_engaged = valobj.GetNumChildren() > 0

    payload = _find_variant(raw) if is_engaged else None
    return is_engaged, payload


def _variant_alternative(valobj):
    """Extract (index, storage SBValue) from a libstdc++ std::variant.

    Layout (libstdc++ 15): `_M_u` is a recursive pair (_M_first / _M_rest),
    each `_M_first` wrapping the alternative value in `_M_storage`; `_M_index`
    selects the active alternative.
    """
    valobj = _raw(valobj)
    index = _child(valobj, "_M_index")
    u = _child(valobj, "_M_u")
    if index is None or u is None:
        return None, None

    index = index.GetValueAsUnsigned(0)
    node = u
    depth = 0
    while node is not None:
        first = _child(node, "_M_first")
        if depth == index:
            return index, _child(first, "_M_storage")
        node = _child(node, "_M_rest")
        depth += 1
    return index, None


def _format_variant(valobj):
    """Render a kernel::value variant as a short string, or None on failure."""
    index, storage = _variant_alternative(valobj)
    if index is None:
        return None

    if index == 0:
        return "unit"
    if index == 1:
        return "bool<{}>".format("true" if storage.GetValueAsUnsigned(0) else "false")
    if index == 2:
        return "int64_t<{}>".format(storage.GetValueAsSigned(0))
    if index == 3:
        return "double<{}>".format(storage.GetValue())
    return "variant<index={}>".format(index)


def value_summary(valobj, internal_dict, options):
    """Summary for clockr::kernel::value."""
    return _format_variant(valobj)


def delay_state_summary(valobj, internal_dict, options):
    """Summary for clockr::kernel::delay_state."""
    valobj = _raw(valobj)
    current = _child(valobj, "current")
    nxt = _child(valobj, "next")
    if current is None or nxt is None:
        return None

    current_text = "nullopt"
    engaged, payload = _optional_payload(current)
    if engaged and payload is not None:
        value = _format_variant(payload)
        if value is not None:
            current_text = value

    next_text = _format_variant(nxt)
    if next_text is None:
        return None

    return "current={}, next={}".format(current_text, next_text)


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        "type summary add -F lldb_formatters.value_summary "
        "'{}'".format(VARIANT_TYPE_NAME)
    )
    debugger.HandleCommand(
        "type summary add -F lldb_formatters.value_summary "
        "'clockr::kernel::value'"
    )
    debugger.HandleCommand(
        "type summary add -F lldb_formatters.delay_state_summary "
        "'clockr::kernel::delay_state'"
    )
