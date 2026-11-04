#pragma once

// server.hpp — the native demo's browser bridge.
//
// Serves the same DAWmulator page the wasm build stages, with one difference:
// app.js opens a WebSocket instead of spawning the worker. The socket protocol
// is the worker protocol verbatim (`ready`/`ui`/`audio`/`block`/`error` down,
// `set`/`loop`/`seek`/`step*`/`auto*`/`blockParams`/`reset` up), so the page
// does not know whether the engine is a wasm worker or a native process.
//
// The uWS loop thread never touches the program: it enqueues commands into the
// host's SPSC queues and reads the host's lock-free snapshots. That keeps the
// set/get quiescence contract on the region threads where it belongs.

#include "host.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace daw::native
{

struct server_impl;

struct server_options
{
    std::string host { "127.0.0.1" };
    std::uint16_t port { 9003 };
    std::string web_root {};
    bool open_browser { false };
};

class server
{
public:
    server(host& engine, server_options options);
    ~server();

    server(const server&) = delete;
    server&
    operator=(const server&) = delete;
    server(server&&) = delete;
    server&
    operator=(server&&) = delete;

    /// Block on the uWebSockets loop until stop() or Ctrl-C. Returns false when
    /// the listen socket could not be opened (nothing was served).
    bool
    run();

    /// Ask the loop to tear itself down. Safe from any thread (and from the
    /// SIGINT handler's flag-driven timer path); the teardown itself runs on
    /// the loop thread.
    void
    stop();

private:
    std::unique_ptr<server_impl> m_impl;
};

} // namespace daw::native
