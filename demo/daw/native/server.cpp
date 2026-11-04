#include "server.hpp"

#include <App.h>

#include <libusockets.h>

#include <atomic>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace daw::native
{

namespace
{
volatile std::sig_atomic_t g_interrupted { 0 };

extern "C" {
static void
on_sigint(int) noexcept
{
    g_interrupted = 1;
}
}

std::string
quoted(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');

    for (const char c : text)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }

    out.push_back('"');
    return out;
}

std::string
number(double value)
{
    if (!std::isfinite(value))
        return "null";

    char buffer[64];
    const auto result { std::to_chars(buffer, buffer + sizeof(buffer), value) };

    if (result.ec != std::errc {})
        return "null";

    return std::string { buffer, result.ptr };
}

std::string
integer(std::int64_t value)
{
    char buffer[32];
    const auto result { std::to_chars(buffer, buffer + sizeof(buffer), value) };

    if (result.ec != std::errc {})
        return "0";

    return std::string { buffer, result.ptr };
}

std::string
optional_number(double value, bool present)
{
    return present ? number(value) : "null";
}

std::string
outputs_json(const audio_snapshot& snapshot)
{
    if (!snapshot.has_outputs)
    {
        return R"({"playhead":null,"midi_count":null,"midi_offset":null,"midi_off_count":null,"voice_gain":null})";
    }

    const auto& outputs { snapshot.outputs };

    return "{\"playhead\":" + number(outputs.playhead) +
           ",\"midi_count\":" + integer(outputs.midi_count) +
           ",\"midi_offset\":" + integer(outputs.midi_offset) +
           ",\"midi_off_count\":" + integer(outputs.midi_off_count) +
           ",\"voice_gain\":" + number(outputs.voice_gain) + "}";
}

std::string
state_json(
    const controls_snapshot& controls,
    const audio_config_snapshot& block,
    bool auto_ui,
    bool auto_audio
)
{
    return "{\"playing\":" +
           std::string { controls.playing ? "true" : "false" } +
           ",\"bpm\":" + number(controls.bpm) +
           ",\"master\":" + number(controls.master) +
           ",\"loop\":{\"start\":" + number(controls.loop_start) +
           ",\"end\":" + number(controls.loop_end) +
           ",\"on\":" + (controls.loop_on ? "true" : "false") + "}" +
           ",\"block\":{\"n\":" +
           integer(static_cast<std::int64_t>(block.n_frames)) +
           ",\"rate\":" + number(block.sample_rate) + "}" +
           ",\"auto\":{\"ui\":" + (auto_ui ? "true" : "false") +
           ",\"audio\":" + (auto_audio ? "true" : "false") + "}}";
}

std::string
state_frame(
    const controls_snapshot& controls,
    const audio_config_snapshot& block,
    bool auto_ui,
    bool auto_audio
)
{
    return "{\"type\":\"state\",\"state\":" +
           state_json(controls, block, auto_ui, auto_audio) + "}";
}

std::string
ready_frame(const host& engine)
{
    const auto controls { engine.controls() };
    const auto ui { engine.ui() };
    const auto audio { engine.audio() };
    const auto block { engine.audio_config() };

    return "{\"type\":\"ready\""
           ",\"engine\":\"native\""
           ",\"audio\":" +
           quoted(engine.null_audio() ? "null" : "device") +
           ",\"defaults\":{\"playing\":" +
           (controls.playing ? "true" : "false") +
           ",\"bpm\":" + number(controls.bpm) +
           ",\"master\":" + number(controls.master) +
           ",\"loop\":{\"start\":" + number(controls.loop_start) +
           ",\"end\":" + number(controls.loop_end) +
           ",\"on\":" + (controls.loop_on ? "true" : "false") + "}}" +
           ",\"block\":{\"n\":" +
           integer(static_cast<std::int64_t>(block.n_frames)) +
           ",\"rate\":" + number(block.sample_rate) + "}" + ",\"uiHz\":60" +
           ",\"outputs\":{\"ui\":{\"playhead_ui\":" +
           optional_number(ui.playhead_ui, ui.has_playhead_ui) +
           "},\"audio\":" + outputs_json(audio) + "}" + ",\"state\":" +
           state_json(controls, block, engine.auto_ui(), engine.auto_audio()) +
           "}";
}

std::string
ui_frame(const ui_snapshot& snapshot)
{
    return "{\"type\":\"ui\",\"seq\":" +
           integer(static_cast<std::int64_t>(snapshot.seq)) +
           ",\"outputs\":{\"playhead_ui\":" +
           optional_number(snapshot.playhead_ui, snapshot.has_playhead_ui) +
           "}}";
}

std::string
audio_frame(const audio_snapshot& snapshot)
{
    return "{\"type\":\"audio\",\"k\":" +
           integer(static_cast<std::int64_t>(snapshot.block)) +
           ",\"block\":{\"start\":" + integer(snapshot.start_sample) +
           ",\"n\":" + integer(static_cast<std::int64_t>(snapshot.n_frames)) +
           "}" + ",\"outputs\":" + outputs_json(snapshot) + "}";
}

std::string
error_frame(std::string_view text)
{
    return "{\"type\":\"error\",\"text\":" + quoted(text) + "}";
}

const char*
error_text(host_error error)
{
    switch (error)
    {
        case host_error::none:
            return "";
        case host_error::audio_tick_threw:
            return "the audio region's tick threw; the block was silenced";
        case host_error::ui_tick_threw:
            return "the ui region's tick threw";
        case host_error::audio_device_failed:
            return "the audio device could not be opened";
        default:
            return "";
    }
}

std::string_view
trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\n' ||
                              value.front() == '\r' || value.front() == '\t'))
        value.remove_prefix(1);

    while (!value.empty() && (value.back() == ' ' || value.back() == '\n' ||
                              value.back() == '\r' || value.back() == '\t'))
        value.remove_suffix(1);

    return value;
}

std::optional<std::string_view>
field(std::string_view json, std::string_view key)
{
    const std::string needle { "\"" + std::string { key } + "\"" };
    const auto found { json.find(needle) };

    if (found == std::string_view::npos)
        return std::nullopt;

    auto pos { found + needle.size() };

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;

    if (pos >= json.size() || json[pos] != ':')
        return std::nullopt;

    ++pos;

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;

    const auto start { pos };
    int depth { 0 };
    bool in_string { false };
    bool escaped { false };

    for (; pos < json.size(); ++pos)
    {
        const char c { json[pos] };

        if (in_string)
        {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;

            continue;
        }

        if (c == '"')
        {
            in_string = true;
        }
        else if (c == '{' || c == '[')
        {
            ++depth;
        }
        else if (c == '}' || c == ']')
        {
            if (depth == 0)
                break;

            --depth;
        }
        else if (c == ',' && depth == 0)
        {
            break;
        }
    }

    return json.substr(start, pos - start);
}

std::optional<std::string>
string_value(std::string_view value)
{
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return std::nullopt;

    std::string out;
    out.reserve(value.size() - 2);

    for (std::size_t i { 1 }; i + 1 < value.size(); ++i)
    {
        const char c { value[i] };

        if (c != '\\')
        {
            out.push_back(c);
            continue;
        }

        ++i;

        if (i + 1 >= value.size())
            return std::nullopt;

        switch (value[i])
        {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            default:
                return std::nullopt;
        }
    }

    return out;
}

bool
bool_value(std::string_view value, bool& out)
{
    if (value == "true")
    {
        out = true;
        return true;
    }

    if (value == "false")
    {
        out = false;
        return true;
    }

    return false;
}

bool
double_value(std::string_view value, double& out)
{
    const std::string text { value };
    char* end { nullptr };
    const double parsed { std::strtod(text.c_str(), &end) };

    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed))
        return false;

    out = parsed;
    return true;
}

bool
int_value(std::string_view value, std::int64_t& out)
{
    const std::string text { value };
    char* end { nullptr };
    const long long parsed { std::strtoll(text.c_str(), &end, 10) };

    if (end == text.c_str() || *end != '\0')
        return false;

    out = static_cast<std::int64_t>(parsed);
    return true;
}

std::string
content_type(std::string_view path)
{
    if (path.ends_with(".html"))
        return "text/html";
    if (path.ends_with(".js"))
        return "text/javascript";
    if (path.ends_with(".css"))
        return "text/css";
    if (path.ends_with(".json"))
        return "application/json";
    if (path.ends_with(".wasm"))
        return "application/wasm";
    if (path.ends_with(".svg"))
        return "image/svg+xml";

    return "application/octet-stream";
}

std::string
read_file(const std::string& path)
{
    std::ifstream file { path, std::ios::binary };

    if (!file)
        return {};

    std::ostringstream contents;
    contents << file.rdbuf();

    return contents.str();
}

void
open_browser(const std::string& url)
{
#if defined(_WIN32)
    std::system(("start \"\" \"" + url + "\"").c_str());
#elif defined(__APPLE__)
    std::system(("open \"" + url + "\" >/dev/null 2>&1 &").c_str());
#else
    std::system(("xdg-open \"" + url + "\" >/dev/null 2>&1 &").c_str());
#endif
}

struct socket_data
{};

using socket_type = uWS::WebSocket<false, true, socket_data>;

bool
same_controls(const controls_snapshot& a, const controls_snapshot& b) noexcept
{
    return a.playing == b.playing && a.bpm == b.bpm && a.master == b.master &&
           a.loop_start == b.loop_start && a.loop_end == b.loop_end &&
           a.loop_on == b.loop_on;
}

struct timer_context
{
    server_impl* self { nullptr };
    us_listen_socket_t* listen_socket { nullptr };
};
} // namespace

struct server_impl
{
    server_impl(host& engine_handle, server_options server_options_value) :
        engine { engine_handle },
        options { std::move(server_options_value) }
    {}

    host& engine;
    server_options options;

    std::vector<socket_type*> connections;
    std::atomic<bool> stopping { false };
    std::atomic<bool> running { false };
    us_loop_t* loop { nullptr };
    uWS::App* app { nullptr };
    us_timer_t* timer_handle { nullptr };

    std::uint64_t last_ui_seq { 0 };
    controls_snapshot last_controls {};
    audio_config_snapshot last_config {};
    bool last_auto_ui { false };
    bool last_auto_audio { false };
    bool has_state { false };

    void
    send(socket_type* socket, std::string frame)
    {
        if (socket != nullptr)
            socket->send(std::move(frame), uWS::OpCode::TEXT, false);
    }

    void
    send_error(socket_type* socket, std::string_view text)
    {
        send(socket, error_frame(text));
    }

    void
    publish(std::string frame)
    {
        if (app != nullptr)
            app->publish("engine", std::move(frame), uWS::OpCode::TEXT, false);
    }

    void
    handle_command(socket_type* socket, std::string_view data)
    {
        const auto type_field { field(data, "type") };

        if (!type_field.has_value())
        {
            send_error(socket, "command has no type");
            return;
        }

        const auto type { string_value(trim(*type_field)) };

        if (!type.has_value())
        {
            send_error(socket, "command type is not a string");
            return;
        }

        if (*type == "set")
        {
            const auto port_field { field(data, "port") };
            const auto value_field { field(data, "value") };

            if (!port_field.has_value() || !value_field.has_value())
            {
                send_error(socket, "set needs port and value");
                return;
            }

            const auto port { string_value(trim(*port_field)) };

            if (!port.has_value())
            {
                send_error(socket, "set port is not a string");
                return;
            }

            ui_command command;

            if (*port == "playing")
            {
                bool on { false };

                if (!bool_value(trim(*value_field), on))
                {
                    send_error(socket, "playing needs a boolean");
                    return;
                }

                command.kind = ui_command_kind::set_playing;
                command.on = on;
            }
            else if (*port == "bpm" || *port == "master")
            {
                double value { 0.0 };

                if (!double_value(trim(*value_field), value))
                {
                    send_error(socket, "bpm/master need a number");
                    return;
                }

                command.kind = *port == "bpm" ? ui_command_kind::set_bpm
                                              : ui_command_kind::set_master;
                command.a = value;
            }
            else
            {
                send_error(
                    socket, "unknown set port (want playing, bpm or master)"
                );
                return;
            }

            if (!engine.send_ui(command))
                send_error(socket, "ui command queue is full");
            return;
        }

        if (*type == "loop")
        {
            const auto start_field { field(data, "start") };
            const auto end_field { field(data, "end") };
            const auto on_field { field(data, "on") };

            double start { 0.0 };
            double end { 0.0 };
            bool on { false };

            if (!start_field.has_value() || !end_field.has_value() ||
                !on_field.has_value() ||
                !double_value(trim(*start_field), start) ||
                !double_value(trim(*end_field), end) ||
                !bool_value(trim(*on_field), on))
            {
                send_error(
                    socket, "loop needs numeric start/end and a boolean on"
                );
                return;
            }

            ui_command command;
            command.kind = ui_command_kind::set_loop;
            command.a = start;
            command.b = end;
            command.on = on;

            if (!engine.send_ui(command))
                send_error(socket, "ui command queue is full");
            return;
        }

        if (*type == "seek")
        {
            const auto beats_field { field(data, "beats") };
            ui_command command;
            command.kind = ui_command_kind::clear_seek;

            if (beats_field.has_value() && trim(*beats_field) != "null")
            {
                double beats { 0.0 };

                if (!double_value(trim(*beats_field), beats))
                {
                    send_error(socket, "seek needs a number or null");
                    return;
                }

                command.kind = ui_command_kind::set_seek;
                command.a = beats;
            }

            if (!engine.send_ui(command))
                send_error(socket, "ui command queue is full");
            return;
        }

        if (*type == "stepUi")
        {
            ui_command command;
            command.kind = ui_command_kind::step;

            if (!engine.send_ui(command))
                send_error(socket, "ui command queue is full");
            return;
        }

        if (*type == "autoUi" || *type == "autoAudio")
        {
            const auto on_field { field(data, "on") };
            bool on { false };

            if (!on_field.has_value() || !bool_value(trim(*on_field), on))
            {
                send_error(socket, "auto needs a boolean on");
                return;
            }

            if (*type == "autoUi")
                engine.set_auto_ui(on);
            else
                engine.set_auto_audio(on);

            return;
        }

        if (*type == "stepAudio")
        {
            audio_command command;
            command.kind = audio_command_kind::step;

            if (!engine.send_audio(command))
                send_error(socket, "audio command queue is full");
            return;
        }

        if (*type == "blockParams")
        {
            const auto n_field { field(data, "n") };
            const auto rate_field { field(data, "rate") };
            std::int64_t n { 0 };
            double rate { 0.0 };

            if (!n_field.has_value() || !rate_field.has_value() ||
                !int_value(trim(*n_field), n) ||
                !double_value(trim(*rate_field), rate) || n <= 0 || n > 65536 ||
                rate <= 0.0)
            {
                send_error(socket, "blockParams needs a positive n and rate");
                return;
            }

            std::string error;

            if (!engine.reconfigure_audio(
                    static_cast<std::uint32_t>(n), rate, error
                ))
                send_error(socket, error);
            return;
        }

        if (*type == "reset")
        {
            std::string error;

            if (!engine.restart(error))
            {
                send_error(socket, error);
                return;
            }

            last_ui_seq = 0;
            has_state = false;
            publish(ready_frame(engine));
            return;
        }

        send_error(socket, "unknown command type");
    }

    void
    on_message(socket_type* socket, std::string_view data)
    {
        try
        {
            handle_command(socket, data);
        }
        catch (const std::exception& error)
        {
            send_error(socket, error.what());
        }
        catch (...)
        {
            send_error(socket, "command failed");
        }
    }

    void
    on_open(socket_type* socket)
    {
        connections.push_back(socket);
        socket->subscribe("engine");
        socket->send(ready_frame(engine), uWS::OpCode::TEXT, true);
    }

    void
    on_close(socket_type* socket)
    {
        std::erase(connections, socket);
    }

    void
    teardown(us_timer_t* timer_arg)
    {
        for (auto* socket : connections)
        {
            if (socket != nullptr)
                socket->close();
        }

        connections.clear();

        auto* context { static_cast<timer_context*>(us_timer_ext(timer_arg)) };

        if (context != nullptr && context->listen_socket != nullptr)
        {
            us_listen_socket_close(0, context->listen_socket);
            context->listen_socket = nullptr;
        }

        us_timer_close(timer_arg);
    }

    void
    on_timer(us_timer_t* timer_arg)
    {
        if (g_interrupted != 0)
            stopping.store(true, std::memory_order_release);

        if (stopping.load(std::memory_order_acquire))
        {
            teardown(timer_arg);
            return;
        }

        if (app == nullptr)
            return;

        const auto error { engine.take_error() };

        if (error != host_error::none)
            publish(error_frame(error_text(error)));

        const auto controls { engine.controls() };
        const auto config { engine.audio_config() };
        const bool auto_ui { engine.auto_ui() };
        const bool auto_audio { engine.auto_audio() };
        const bool state_changed {
            !has_state || !same_controls(controls, last_controls) ||
            config.n_frames != last_config.n_frames ||
            config.sample_rate != last_config.sample_rate ||
            auto_ui != last_auto_ui || auto_audio != last_auto_audio
        };

        if (state_changed)
        {
            last_controls = controls;
            last_config = config;
            last_auto_ui = auto_ui;
            last_auto_audio = auto_audio;
            has_state = true;
            publish(state_frame(controls, config, auto_ui, auto_audio));
        }

        const auto ui { engine.ui() };

        if (ui.seq != last_ui_seq)
        {
            last_ui_seq = ui.seq;
            publish(ui_frame(ui));
        }

        audio_snapshot audio_event;

        while (engine.pop_audio_event(audio_event))
            publish(audio_frame(audio_event));
    }

    void
    serve_static(uWS::HttpResponse<false>* response, uWS::HttpRequest* request)
    {
        std::string path { request->getUrl() };
        const auto query { path.find('?') };

        if (query != std::string::npos)
            path.resize(query);

        if (path.empty() || path == "/")
        {
            response->writeStatus("302 Found")
                ->writeHeader("Location", "/index.html?native=1")
                ->end();
            return;
        }

        if (path.find("..") != std::string::npos)
        {
            response->writeStatus("404 Not Found")->end("not found");
            return;
        }

        auto file { read_file(options.web_root + path) };

        if (path == "/index.html")
        {
            constexpr std::string_view marker {
                "<script src=\"./app.js\"></script>"
            };
            const auto at { file.find(marker) };

            if (at != std::string::npos)
                file.insert(
                    at, "<script>window.CLOCKR_NATIVE=true;</script>\n"
                );
        }

        if (file.empty())
        {
            response->writeStatus("404 Not Found")->end("not found");
            return;
        }

        response->writeHeader("Content-Type", content_type(path))
            ->writeHeader("Cache-Control", "no-store")
            ->end(file);
    }

    bool
    run()
    {
        g_interrupted = 0;
        std::signal(SIGINT, &on_sigint);

        us_listen_socket_t* listen_socket { nullptr };

        auto app_object {
            uWS::App()
                .get(
                    "/*",
                    [this](
                        uWS::HttpResponse<false>* response,
                        uWS::HttpRequest* request
                    ) {
                        serve_static(response, request);
                    }
                )
                .ws<socket_data>(
                    "/ws",
                    {
                        .compression = uWS::DISABLED,
                        .maxPayloadLength = 64 * 1024,
                        .idleTimeout = 120,
                        .maxBackpressure = 4 * 1024 * 1024,
                        .closeOnBackpressureLimit = true,
                        .sendPingsAutomatically = true,
                        .open =
                            [this](socket_type* socket) {
                                on_open(socket);
                            },
                        .message =
                            [this](
                                socket_type* socket,
                                std::string_view data,
                                uWS::OpCode
                            ) {
                                on_message(socket, data);
                            },
                        .drain =
                            [](socket_type*) {
                            },
                        .ping =
                            [](socket_type*, std::string_view) {
                            },
                        .pong =
                            [](socket_type*, std::string_view) {
                            },
                        .close =
                            [this](socket_type* socket, int, std::string_view) {
                                on_close(socket);
                            },
                    }
                )
                .listen(
                    options.host,
                    options.port,
                    LIBUS_LISTEN_EXCLUSIVE_PORT,
                    [&listen_socket](us_listen_socket_t* token) {
                        listen_socket = token;
                    }
                )
        };

        if (listen_socket == nullptr)
        {
            std::cerr << "[dawmulator] failed to listen on " << options.host
                      << ":" << options.port << "\n";
            return false;
        }

        std::string browser_host { options.host };

        if (browser_host == "0.0.0.0" || browser_host == "::")
            browser_host = "127.0.0.1";

        const std::string url { "http://" + browser_host + ":" +
                                std::to_string(options.port) +
                                "/index.html?native=1" };

        std::cout << "[dawmulator] serving the DAW page on " << url
                  << " (web root " << options.web_root << ")" << std::endl;
        std::cout << "[dawmulator] audio: "
                  << (engine.null_audio() ? "null backend (silent)" : "device")
                  << " — Ctrl-C to stop" << std::endl;

        if (options.open_browser)
            open_browser(url);

        app = &app_object;

        auto* loop_pointer { reinterpret_cast<us_loop_t*>(uWS::Loop::get()) };
        loop = loop_pointer;
        running.store(true, std::memory_order_release);

        last_ui_seq = engine.ui().seq;
        last_controls = engine.controls();
        last_config = engine.audio_config();
        last_auto_ui = engine.auto_ui();
        last_auto_audio = engine.auto_audio();
        has_state = true;

        auto* timer_pointer {
            us_create_timer(loop_pointer, 0, sizeof(timer_context))
        };

        if (timer_pointer != nullptr)
        {
            auto* timer_ext {
                static_cast<timer_context*>(us_timer_ext(timer_pointer))
            };
            timer_ext->self = this;
            timer_ext->listen_socket = listen_socket;

            timer_handle = timer_pointer;

            us_timer_set(
                timer_pointer,
                [](us_timer_t* timer_arg) {
                    auto* callback_ext {
                        static_cast<timer_context*>(us_timer_ext(timer_arg))
                    };

                    if (callback_ext != nullptr &&
                        callback_ext->self != nullptr)
                        callback_ext->self->on_timer(timer_arg);
                },
                16,
                16
            );
        }

        app_object.run();

        running.store(false, std::memory_order_release);
        loop = nullptr;
        timer_handle = nullptr;
        app = nullptr;

        return true;
    }
};

server::server(host& engine, server_options options) :
    m_impl { std::make_unique<server_impl>(engine, std::move(options)) }
{}

server::~server()
{
    stop();
}

bool
server::run()
{
    return m_impl->run();
}

void
server::stop()
{
    m_impl->stopping.store(true, std::memory_order_release);

    if (m_impl->running.load(std::memory_order_acquire))
        if (auto* loop { m_impl->loop })
            us_wakeup_loop(loop);
}

} // namespace daw::native
