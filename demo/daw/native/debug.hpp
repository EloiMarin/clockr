#pragma once

// debug.hpp — the live MCP bridge.
//
// This is the "host that serves the debugger" the agent workflow expects: the
// DAWmulator's program and recorder wrapped in the debugger's `debug::server`,
// with the command hook attached to the two engines that tick it. The page and
// the JSON bridge are untouched; an MCP client connects to the same live
// program on a separate debug port (9002 by default) and speaks the normal
// graph/trace/injection protocol.
//
// The bridge owns the recorder, the server, the hook and the two debug
// engines. It is constructed after the program and destroyed before it, so
// recorder/command teardown can still release pooled user values.

#include "clockr/kernel/engine.hpp"
#include "clockr/kernel/program.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace daw::native
{

namespace kernel = clockr::kernel;

struct live_debug_options
{
    std::string host { "127.0.0.1" };
    std::uint16_t port { 9002 };
    std::string web_root {}; // empty: the debugger's own web/ directory
    std::string example_name { "dawmulator" };
};

class live_debug
{
public:
    live_debug(kernel::program& program, live_debug_options options);
    ~live_debug();

    live_debug(const live_debug&) = delete;
    live_debug&
    operator=(const live_debug&) = delete;
    live_debug(live_debug&&) = delete;
    live_debug&
    operator=(live_debug&&) = delete;

    /// Start the debug server's event-loop thread. The server may report a
    /// failed listen on its own stderr; the demo keeps running either way.
    bool
    start(std::string& error);

    /// Stop and join the server thread. Safe to call more than once.
    void
    stop();

    kernel::engine&
    ui_engine() noexcept;

    kernel::engine&
    audio_engine() noexcept;

private:
    struct impl;
    std::unique_ptr<impl> m_impl;
};

} // namespace daw::native
