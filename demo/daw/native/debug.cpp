#include "debug.hpp"

#include "server/commands.hpp"
#include "server/recorder.hpp"
#include "server/server.hpp"

#include "clockr/kernel/debug_engine.hpp"

#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace daw::native
{

namespace debug = clockr::debug;

namespace
{
#ifndef DAW_DEBUG_WEB_ROOT
constexpr const char* kFallbackWebRoot { "tools/web-debugger/web" };
#endif
} // namespace

struct live_debug::impl
{
    impl(kernel::program& program_ref, live_debug_options debug_options) :
        program { program_ref },
        options { std::move(debug_options) }
    {
        debug::server_options server_options;

        server_options.host = options.host;
        server_options.port = options.port;

        if (!options.web_root.empty())
            server_options.web_root = options.web_root;
#ifdef DAW_DEBUG_WEB_ROOT
        else
            server_options.web_root = DAW_DEBUG_WEB_ROOT;
#else
        else
            server_options.web_root = kFallbackWebRoot;
#endif

        std::vector<debug::example> examples;
        examples.push_back(
            debug::example { options.example_name, program, recorder }
        );

        server = std::make_unique<debug::server>(
            std::move(examples), std::move(server_options)
        );

        commands = server->commands_for(options.example_name);

        if (commands == nullptr)
            throw std::runtime_error {
                "debug server has no commands for the example"
            };

        hook = std::make_unique<debug::command_hook>(*commands);

        ui_engine = std::make_unique<kernel::debug_engine>(recorder);
        audio_engine = std::make_unique<kernel::debug_engine>(recorder);

        ui_engine->set_step_hook(hook.get());
        audio_engine->set_step_hook(hook.get());
    }

    kernel::program& program;
    live_debug_options options;

    debug::trace_recorder recorder;
    std::unique_ptr<debug::server> server;
    debug::example_commands* commands { nullptr };
    std::unique_ptr<debug::command_hook> hook;
    std::unique_ptr<kernel::debug_engine> ui_engine;
    std::unique_ptr<kernel::debug_engine> audio_engine;
    std::thread thread;
};

live_debug::live_debug(kernel::program& program, live_debug_options options) :
    m_impl { std::make_unique<impl>(program, std::move(options)) }
{}

live_debug::~live_debug()
{
    stop();
}

bool
live_debug::start(std::string& error)
{
    if (m_impl->thread.joinable())
        return true;

    try
    {
        m_impl->thread = std::thread { [this] {
            m_impl->server->run();
        } };
    }
    catch (const std::exception& failure)
    {
        error = std::string { "failed to start the debug server: " } +
                failure.what();
        return false;
    }

    return true;
}

void
live_debug::stop()
{
    if (m_impl->server != nullptr)
        m_impl->server->stop();

    if (m_impl->thread.joinable())
        m_impl->thread.join();
}

kernel::engine&
live_debug::ui_engine() noexcept
{
    return *m_impl->ui_engine;
}

kernel::engine&
live_debug::audio_engine() noexcept
{
    return *m_impl->audio_engine;
}

} // namespace daw::native
