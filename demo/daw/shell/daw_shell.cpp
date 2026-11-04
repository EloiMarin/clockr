// DAWmulator embind shell — the dumb shell contract (set/tick/get) exposed to page
// JavaScript. Built only for Emscripten (demo/daw/shell); the engine never
// knows it is remote — these are the same calls the C++ tests make.
//
// Numbers cross as doubles and bools only: JS has no int64, and embind i64
// would force BigInt on callers. Sample counts stay exact — everything here
// is far below 2^53.
//
// Set and step are separate calls, mirroring the C++ contract exactly:
// set<BlockDesc>("block", ...) then step(audio). A region that has not
// produced a value yet (a ui tick before the first audio tick, or a fresh
// program) still reads as monostate; the getters report that as NaN rather
// than throwing, so a shell can poll freely — the strict read stays
// available through the C++ tests.
//
// The same module also carries the in-browser debugger's producer side. When
// the page asks (the debugger's local transport sends `resume`), the shell
// switches to `debug_engine` instances and drains a `trace_recorder`; the
// graph description and event batches are built directly as JavaScript
// values, so the browser build needs neither protobuf nor sockets. With no
// debugger attached the release engines tick, so the page pays nothing for a
// feature it is not showing.

#include "daw/dawmulator.hpp"
#include "daw/network.hpp"

#include "server/recorder.hpp"

#include "clockr/kernel/debug_engine.hpp"
#include "clockr/kernel/descriptor.hpp"
#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/kernel/topology.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{

namespace debug = clockr::debug;
namespace kernel = clockr::kernel;

using clockr::kernel::clock_tag;
using clockr::kernel::value;

constexpr int max_struct_depth { 8 };

const char*
clock_name(clock_tag clock) noexcept
{
    switch (clock)
    {
        case clock_tag::audio:
            return "audio";
        case clock_tag::ui:
            return "ui";
        case clock_tag::worker:
            return "worker";
        default:
            return "unknown";
    }
}

const char*
node_kind_name(kernel::node_kind kind) noexcept
{
    switch (kind)
    {
        case kernel::node_kind::input:
            return "input";
        case kernel::node_kind::output:
            return "output";
        case kernel::node_kind::internal:
            return "internal";
        case kernel::node_kind::delay:
            return "delay";
        default:
            return "unknown";
    }
}

const char*
trace_kind_name(kernel::trace_event_kind kind) noexcept
{
    switch (kind)
    {
        case kernel::trace_event_kind::region_step_begin:
            return "region_step_begin";
        case kernel::trace_event_kind::region_step_end:
            return "region_step_end";
        case kernel::trace_event_kind::node_tick_begin:
            return "node_tick_begin";
        case kernel::trace_event_kind::node_tick_end:
            return "node_tick_end";
        case kernel::trace_event_kind::node_tick_throw:
            return "node_tick_throw";
        default:
            return "unknown";
    }
}

emscripten::val
js_value(const value& payload);

emscripten::val
js_struct(const kernel::user_type* type, const void* payload, int depth);

emscripten::val
js_field(const void* base, const kernel::field_desc& field, int depth)
{
    if (field.kind == kernel::field_kind::nested)
        return js_struct(
            field.nested,
            static_cast<const std::byte*>(base) + field.offset,
            depth + 1
        );

    if (field.kind == kernel::field_kind::text)
    {
        emscripten::val out { emscripten::val::object() };
        out.set("kind", "string");
        out.set("value", std::string { kernel::read_text_field(base, field) });
        return out;
    }

    const auto field_value { kernel::read_field(base, field) };

    if (field.kind == kernel::field_kind::enumeration)
        if (const auto* number { std::get_if<std::int64_t>(&field_value) })
            if (const auto* name { kernel::enum_name(field, *number) })
            {
                emscripten::val out { emscripten::val::object() };
                out.set("kind", "string");
                out.set("value", std::string { name });
                return out;
            }

    return js_value(field_value);
}

emscripten::val
js_struct(const kernel::user_type* type, const void* payload, int depth)
{
    emscripten::val out { emscripten::val::object() };
    out.set("typeId", static_cast<double>(type != nullptr ? type->id : 0));

    emscripten::val fields { emscripten::val::array() };

    if (type != nullptr && payload != nullptr && depth < max_struct_depth)
        for (const auto& field : type->fields)
            fields.call<void>("push", js_field(payload, field, depth + 1));

    out.set("fields", fields);
    return out;
}

emscripten::val
js_value(const value& payload)
{
    emscripten::val out { emscripten::val::object() };

    if (const auto* flag { std::get_if<bool>(&payload) })
    {
        out.set("kind", "bool");
        out.set("value", *flag);
        return out;
    }

    if (const auto* number { std::get_if<std::int64_t>(&payload) })
    {
        out.set("kind", "int");
        out.set("value", static_cast<double>(*number));
        return out;
    }

    if (const auto* number { std::get_if<double>(&payload) })
    {
        out.set("kind", "double");
        out.set("value", *number);
        return out;
    }

    if (const auto* inline_value { std::get_if<kernel::user_inline>(&payload) })
    {
        out.set("kind", "struct");
        out.set(
            "value",
            js_struct(
                inline_value->type,
                inline_value->bytes.data(),
                0
            )
        );
        return out;
    }

    if (const auto* reference { std::get_if<kernel::user_ref>(&payload) })
    {
        const auto* cell { reference->cell };

        out.set("kind", "struct");
        out.set(
            "value",
            js_struct(
                cell != nullptr ? cell->type : nullptr,
                cell != nullptr ? kernel::user_payload(cell) : nullptr,
                0
            )
        );
        return out;
    }

    out.set("kind", "unit");
    return out;
}

emscripten::val
js_graph(const kernel::program& program)
{
    const auto graph { program.descriptor() };

    emscripten::val out { emscripten::val::object() };
    emscripten::val nodes { emscripten::val::array() };

    for (const auto& node : kernel::nodes(graph))
    {
        const auto node_id { kernel::id(node) };
        const auto label { kernel::label(graph, node_id) };

        emscripten::val entry { emscripten::val::object() };
        entry.set("id", static_cast<double>(node_id));
        entry.set("kind", node_kind_name(kernel::kind(node)));
        entry.set("clock", clock_name(kernel::clock(node)));
        entry.set(
            "label",
            label.combinator != nullptr ? std::string { label.combinator }
                                        : std::string {}
        );
        entry.set(
            "port",
            label.port_name != nullptr ? std::string { label.port_name }
                                       : std::string {}
        );

        emscripten::val fan_in { emscripten::val::array() };

        for (const auto producer : kernel::fan_in(graph, node_id))
            fan_in.call<void>("push", static_cast<double>(producer));

        entry.set("fanIn", fan_in);
        nodes.call<void>("push", entry);
    }

    out.set("nodes", nodes);

    emscripten::val sharing { emscripten::val::array() };

    for (const auto& point : program.topology().crossings)
    {
        emscripten::val crossing { emscripten::val::object() };
        crossing.set("from", static_cast<double>(point.from));
        crossing.set("to", static_cast<double>(point.to));
        crossing.set(
            "policy",
            point.policy == clockr::conc::policy::queued ? "queued" : "latest"
        );
        sharing.call<void>("push", crossing);
    }

    out.set("sharingPoints", sharing);

    emscripten::val inputs { emscripten::val::array() };

    for (std::size_t i { 0 }; i < graph.input_ids.size(); ++i)
    {
        emscripten::val port { emscripten::val::object() };
        port.set("nodeId", static_cast<double>(graph.input_ids[i]));
        port.set(
            "name",
            graph.input_names[i] != nullptr ? std::string { graph.input_names[i] }
                                            : std::string {}
        );
        inputs.call<void>("push", port);
    }

    out.set("inputs", inputs);

    emscripten::val outputs { emscripten::val::array() };

    for (std::size_t i { 0 }; i < graph.output_ids.size(); ++i)
    {
        emscripten::val port { emscripten::val::object() };
        port.set("nodeId", static_cast<double>(graph.output_ids[i]));
        port.set(
            "name",
            graph.output_names[i] != nullptr
                ? std::string { graph.output_names[i] }
                : std::string {}
        );
        outputs.call<void>("push", port);
    }

    out.set("outputs", outputs);

    emscripten::val types { emscripten::val::array() };

    for (const auto* type : program.types())
    {
        emscripten::val schema { emscripten::val::object() };
        schema.set("id", static_cast<double>(type->id));
        schema.set(
            "name",
            type->name != nullptr ? std::string { type->name } : std::string {}
        );

        emscripten::val fields { emscripten::val::array() };

        for (const auto& field : type->fields)
        {
            emscripten::val field_schema { emscripten::val::object() };
            field_schema.set(
                "name",
                field.name != nullptr ? std::string { field.name }
                                      : std::string {}
            );
            field_schema.set(
                "kind",
                std::string { kernel::field_kind_text(field.kind) }
            );
            field_schema.set(
                "typeId",
                static_cast<double>(
                    field.kind == kernel::field_kind::nested
                        && field.nested != nullptr
                        ? field.nested->id
                        : 0
                )
            );
            fields.call<void>("push", field_schema);
        }

        schema.set("fields", fields);
        types.call<void>("push", schema);
    }

    out.set("types", types);
    out.set("name", std::string { "dawmulator" });

    return out;
}

emscripten::val
js_event(const debug::recorded_event& recorded)
{
    const auto& event { recorded.event };

    emscripten::val out { emscripten::val::object() };
    out.set("sequence", static_cast<double>(event.sequence));
    out.set("stampNs", static_cast<double>(recorded.stamp_ns));
    out.set("kind", trace_kind_name(event.kind));
    out.set("nodeId", static_cast<double>(event.node_id));
    out.set("clock", clock_name(event.clock));
    out.set("epoch", static_cast<double>(event.epoch));

    switch (event.kind)
    {
        case kernel::trace_event_kind::node_tick_end:
            out.set("value", js_value(event.payload));
            out.set("durationNs", static_cast<double>(event.duration_ns));
            break;

        case kernel::trace_event_kind::node_tick_throw:
            out.set("durationNs", static_cast<double>(event.duration_ns));
            break;

        case kernel::trace_event_kind::region_step_end:
            if (const auto* threw { std::get_if<bool>(&event.payload) })
                out.set("threw", *threw);
            out.set("durationNs", static_cast<double>(event.duration_ns));
            break;

        case kernel::trace_event_kind::region_step_begin:
        case kernel::trace_event_kind::node_tick_begin:
        default:
            break;
    }

    return out;
}

class dawmulator
{
public:
    dawmulator() :
        m_program { daw::dawmulator_recipe() }
    {
        m_recorder.set_enabled(false);
    }

    /// Level the intent ports to the DAWmulator defaults and tick ui once —
    /// the shared construction (daw/dawmulator.hpp), also used by the
    /// debugger's `daw` example. A fresh instance stays unset (NaN reads)
    /// until this runs, so shells keep explicit control of init.
    void
    init_defaults()
    {
        daw::init_dawmulator(m_program, *m_ui_engine);
    }

    void
    set_playing(bool playing)
    {
        m_program.set("playing", playing);
    }

    void
    set_bpm(double bpm)
    {
        m_program.set("bpm", bpm);
    }

    void
    set_loop(double start, double end, bool on)
    {
        m_program.set<daw::LoopRegion>("loop", start, end, on);
    }

    void
    set_master(double master)
    {
        m_program.set("master", master);
    }

    void
    set_seek(double beats)
    {
        m_program.set("seek", clockr::shell::to_value(std::optional<double> { beats }));
    }

    void
    clear_seek()
    {
        m_program.set("seek", value {});
    }

    void
    set_block(double block_start, double n_frames, double sample_rate)
    {
        m_program.set<daw::BlockDesc>(
            "block",
            static_cast<std::int64_t>(block_start),
            static_cast<std::int64_t>(n_frames),
            sample_rate);
    }

    void
    tick_ui()
    {
        m_ui_engine->step(m_program, clock_tag::ui);
    }

    void
    tick_audio()
    {
        m_audio_engine->step(m_program, clock_tag::audio);
    }

    double
    playhead() const
    {
        return read_double("playhead");
    }

    double
    playhead_ui() const
    {
        return read_double("playhead_ui");
    }

    double
    midi_count() const
    {
        return read_int64("midi_count");
    }

    double
    midi_offset() const
    {
        return read_int64("midi_offset");
    }

    double
    midi_off_count() const
    {
        return read_int64("midi_off_count");
    }

    double
    voice_gain() const
    {
        return read_double("voice_gain");
    }

    // Input read-back for the page's controlled widgets. The ui region is
    // single-threaded here, so these are the same boundary values the
    // engine will consume on the next tick.
    bool
    playing() const
    {
        return read_input_bool("playing");
    }

    double
    bpm() const
    {
        return read_input_double("bpm");
    }

    double
    master() const
    {
        return read_input_double("master");
    }

    double
    loop_start() const
    {
        return read_input_loop().start;
    }

    double
    loop_end() const
    {
        return read_input_loop().end;
    }

    bool
    loop_on() const
    {
        return read_input_loop().enabled;
    }

    // --- in-browser debugger producer -------------------------------------

    /// Switch the ticking engines between release and traced. Enabling moves
    /// both regions to their `debug_engine`; disabling drops anything queued
    /// and returns to the release engines, so a hidden debugger costs no
    /// trace work.
    void
    debug_capture(bool enabled)
    {
        if (enabled == m_capturing)
            return;

        m_capturing = enabled;

        if (enabled)
        {
            m_recorder.set_enabled(true);
            m_ui_engine = &m_debug_ui_engine;
            m_audio_engine = &m_debug_audio_engine;
            return;
        }

        m_recorder.set_enabled(false);
        m_recorder.discard_pending();
        m_ui_engine = &m_release_engine;
        m_audio_engine = &m_release_engine;
    }

    /// The static network description, in the same shape the debugger's
    /// protobuf decoder produces (`clockr_debug.proto` GraphDescription).
    emscripten::val
    debug_graph() const
    {
        return js_graph(m_program);
    }

    /// Move every queued trace event out of the recorder and return one
    /// `EventBatch`-shaped value per clock, matching the debugger's decoder.
    emscripten::val
    debug_drain()
    {
        std::vector<debug::recorded_event> events;
        events.reserve(1024);

        m_recorder.drain(events, 16384);

        // Each queued event owns one reference to its struct payload; spend it
        // after the JavaScript copies have been built, on the throwing path
        // too. Same hand-back as the native server.
        struct payload_release
        {
            std::vector<debug::recorded_event>& drained;

            ~payload_release()
            {
                for (const auto& recorded : drained)
                    kernel::release(recorded.event.payload);
            }
        } release_payloads { events };

        const auto dropped { m_recorder.take_dropped() };

        std::vector<emscripten::val> batches;
        std::array<std::size_t, kernel::clock_tag_count> counts {};

        batches.reserve(kernel::clock_tag_count);

        for (std::size_t i { 0 }; i < kernel::clock_tag_count; ++i)
            batches.push_back(emscripten::val::array());

        for (const auto& recorded : events)
        {
            const auto slot { kernel::clock_tag_index(recorded.event.clock) };

            batches[slot].call<void>("push", js_event(recorded));
            ++counts[slot];
        }

        emscripten::val out { emscripten::val::array() };

        for (std::size_t slot { 0 }; slot < kernel::clock_tag_count; ++slot)
        {
            if (counts[slot] == 0 && dropped[slot] == 0)
                continue;

            emscripten::val batch { emscripten::val::object() };
            batch.set(
                "clock",
                clock_name(static_cast<clock_tag>(slot))
            );
            batch.set("dropped", static_cast<double>(dropped[slot]));
            batch.set("events", batches[slot]);
            out.call<void>("push", batch);
        }

        return out;
    }

private:
    // NaN is "not produced yet" — no DAW output can legitimately be NaN.
    double
    read_double(const char* port) const
    {
        const value v { m_program.get(port) };

        if (const auto* d { std::get_if<double>(&v) })
            return *d;

        return std::numeric_limits<double>::quiet_NaN();
    }

    double
    read_int64(const char* port) const
    {
        const value v { m_program.get(port) };

        if (const auto* i { std::get_if<std::int64_t>(&v) })
            return static_cast<double>(*i);

        return std::numeric_limits<double>::quiet_NaN();
    }

    double
    read_input_double(const char* port) const
    {
        const value v { m_program.get_input(port) };

        if (const auto* d { std::get_if<double>(&v) })
            return *d;

        return std::numeric_limits<double>::quiet_NaN();
    }

    bool
    read_input_bool(const char* port) const
    {
        const value v { m_program.get_input(port) };

        if (const auto* b { std::get_if<bool>(&v) })
            return *b;

        return false;
    }

    daw::LoopRegion
    read_input_loop() const
    {
        try
        {
            return clockr::kernel::get_input<daw::LoopRegion>(m_program, "loop");
        }
        catch (...)
        {
            return daw::LoopRegion {};
        }
    }

    kernel::program m_program;
    debug::trace_recorder m_recorder;
    kernel::release_engine m_release_engine;
    kernel::debug_engine m_debug_ui_engine { m_recorder };
    kernel::debug_engine m_debug_audio_engine { m_recorder };
    kernel::engine* m_ui_engine { &m_release_engine };
    kernel::engine* m_audio_engine { &m_release_engine };
    bool m_capturing { false };
};

} // namespace

// embind registers through static registrars by design; silence the two
// -Weverything diagnostics inherent to that mechanism (same class as any
// static test registration — the bindings below are all used).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(dawmulator_shell)
{
    emscripten::class_<dawmulator>("Dawmulator")
        .constructor()
        .function("initDefaults", &dawmulator::init_defaults)
        .function("setPlaying", &dawmulator::set_playing)
        .function("setBpm", &dawmulator::set_bpm)
        .function("setLoop", &dawmulator::set_loop)
        .function("setMaster", &dawmulator::set_master)
        .function("setSeek", &dawmulator::set_seek)
        .function("clearSeek", &dawmulator::clear_seek)
        .function("setBlock", &dawmulator::set_block)
        .function("tickUi", &dawmulator::tick_ui)
        .function("tickAudio", &dawmulator::tick_audio)
        .function("playhead", &dawmulator::playhead)
        .function("playheadUi", &dawmulator::playhead_ui)
        .function("midiCount", &dawmulator::midi_count)
        .function("midiOffset", &dawmulator::midi_offset)
        .function("midiOffCount", &dawmulator::midi_off_count)
        .function("voiceGain", &dawmulator::voice_gain)
        .function("playing", &dawmulator::playing)
        .function("bpm", &dawmulator::bpm)
        .function("master", &dawmulator::master)
        .function("loopStart", &dawmulator::loop_start)
        .function("loopEnd", &dawmulator::loop_end)
        .function("loopOn", &dawmulator::loop_on)
        .function("debugCapture", &dawmulator::debug_capture)
        .function("debugGraph", &dawmulator::debug_graph)
        .function("debugDrain", &dawmulator::debug_drain);
}
#pragma clang diagnostic pop
