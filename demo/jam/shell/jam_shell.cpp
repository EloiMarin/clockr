#include "jam/network.hpp"
#include "jam/patch.hpp"
#include "jam/score.hpp"

#include "daw/transport.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/release_engine.hpp"
#include "clockr/kernel/value.hpp"
#include "clockr/replay/compare.hpp"
#include "clockr/replay/native.hpp"
#include "clockr/replay/replayer.hpp"
#include "clockr/replay/scenario.hpp"
#include "clockr/replay/session.hpp"
#include "clockr/replay/value_codec.hpp"

#include <emscripten/bind.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{

namespace kernel = clockr::kernel;
namespace replay = clockr::replay;

using clockr::kernel::clock_tag;
using clockr::kernel::value;

constexpr std::size_t max_json_bytes { 16U * 1024U * 1024U };
constexpr std::size_t max_json_depth { 64 };
constexpr std::size_t max_json_items { 1U << 20U };
constexpr std::size_t max_scenario_entries { 200000 };
constexpr std::size_t max_hex_bytes { 65536 };
constexpr std::size_t max_note_length { 4096 };
constexpr std::size_t max_branch_ticks { 4096 };
constexpr std::size_t max_step_blocks { 4096 };
constexpr std::size_t max_branch_batch { 32 };

constexpr std::int64_t frames_per_block { 1024 };
constexpr double sample_rate { 48000.0 };
constexpr double loop_end_default { 16.0 };
constexpr double master_default { 0.8 };
constexpr std::size_t ui_every_blocks { 8 };

constexpr std::size_t output_playhead { 0 };
constexpr std::size_t output_events { 1 };
constexpr std::size_t output_voices { 2 };
constexpr std::size_t output_mix { 3 };
constexpr std::size_t output_playhead_ui { 4 };

struct json_value
{
    enum class kind_t
    {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    kind_t kind { kind_t::null };
    bool boolean { false };
    double number { 0.0 };
    std::string text;
    std::vector<json_value> items;
    std::vector<std::pair<std::string, json_value>> members;

    [[nodiscard]] const json_value*
    find(std::string_view key) const noexcept
    {
        for (const auto& member : members)
            if (member.first == key)
                return &member.second;

        return nullptr;
    }

    [[nodiscard]] bool
    is_object() const noexcept
    {
        return kind == kind_t::object;
    }

    [[nodiscard]] bool
    is_array() const noexcept
    {
        return kind == kind_t::array;
    }

    [[nodiscard]] bool
    is_string() const noexcept
    {
        return kind == kind_t::string;
    }

    [[nodiscard]] bool
    is_number() const noexcept
    {
        return kind == kind_t::number;
    }

    [[nodiscard]] bool
    is_boolean() const noexcept
    {
        return kind == kind_t::boolean;
    }

    [[nodiscard]] bool
    is_null() const noexcept
    {
        return kind == kind_t::null;
    }
};

class json_parser
{
public:
    explicit json_parser(std::string_view text) :
        m_text { text }
    {}

    bool
    parse(json_value& out, std::string& error)
    {
        if (m_text.size() > max_json_bytes)
        {
            error = "JSON input exceeds the 16 MiB limit";
            return false;
        }

        skip_space();

        if (!parse_value(out, 0))
        {
            error = m_error.empty() ? "malformed JSON" : m_error;
            return false;
        }

        skip_space();

        if (m_pos != m_text.size())
        {
            fail("trailing characters after JSON value");
            error = m_error;
            return false;
        }

        return true;
    }

private:
    bool
    parse_value(json_value& out, std::size_t depth)
    {
        if (depth > max_json_depth)
            return fail("JSON nesting is too deep");

        if (m_pos >= m_text.size())
            return fail("unexpected end of JSON");

        const auto ch { m_text[m_pos] };

        switch (ch)
        {
            case '{':
                return parse_object(out, depth);
            case '[':
                return parse_array(out, depth);
            case '"':
                out.kind = json_value::kind_t::string;
                return parse_string(out.text);
            case 't':
                return parse_literal("true", out, true);
            case 'f':
                return parse_literal("false", out, false);
            case 'n':
                return parse_null(out);
            default:
                if (ch == '-' || (ch >= '0' && ch <= '9'))
                    return parse_number(out);

                return fail("unsupported JSON token");
        }
    }

    bool
    parse_literal(std::string_view literal, json_value& out, bool boolean)
    {
        if (m_text.substr(m_pos, literal.size()) != literal)
            return fail("invalid JSON literal");

        m_pos += literal.size();
        out.kind = json_value::kind_t::boolean;
        out.boolean = boolean;
        return true;
    }

    bool
    parse_null(json_value& out)
    {
        if (m_text.substr(m_pos, 4) != "null")
            return fail("invalid JSON literal");

        m_pos += 4;
        out.kind = json_value::kind_t::null;
        return true;
    }

    bool
    parse_number(json_value& out)
    {
        const auto begin { m_pos };

        if (m_text[m_pos] == '-')
            ++m_pos;

        if (m_pos >= m_text.size())
            return fail("truncated JSON number");

        if (m_text[m_pos] == '0')
            ++m_pos;
        else if (m_text[m_pos] >= '1' && m_text[m_pos] <= '9')
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' &&
                   m_text[m_pos] <= '9')
                ++m_pos;
        else
            return fail("invalid JSON number");

        if (m_pos < m_text.size() && m_text[m_pos] == '.')
        {
            ++m_pos;

            if (m_pos >= m_text.size() || m_text[m_pos] < '0' ||
                m_text[m_pos] > '9')
                return fail("invalid JSON fraction");

            while (m_pos < m_text.size() && m_text[m_pos] >= '0' &&
                   m_text[m_pos] <= '9')
                ++m_pos;
        }

        if (m_pos < m_text.size() &&
            (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
        {
            ++m_pos;

            if (m_pos < m_text.size() &&
                (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
                ++m_pos;

            if (m_pos >= m_text.size() || m_text[m_pos] < '0' ||
                m_text[m_pos] > '9')
                return fail("invalid JSON exponent");

            while (m_pos < m_text.size() && m_text[m_pos] >= '0' &&
                   m_text[m_pos] <= '9')
                ++m_pos;
        }

        const std::string token { m_text.substr(begin, m_pos - begin) };
        char* end { nullptr };
        const double number { std::strtod(token.c_str(), &end) };

        if (end == nullptr || *end != '\0')
            return fail("invalid JSON number");

        out.kind = json_value::kind_t::number;
        out.number = number;
        return true;
    }

    bool
    parse_string(std::string& out)
    {
        if (m_text[m_pos] != '"')
            return fail("expected JSON string");

        ++m_pos;
        out.clear();
        out.reserve(32);

        while (m_pos < m_text.size())
        {
            const auto ch { m_text[m_pos++] };

            if (ch == '"')
                return true;

            if (static_cast<unsigned char>(ch) < 0x20U)
                return fail("control character in JSON string");

            if (ch != '\\')
            {
                out.push_back(ch);
                continue;
            }

            if (m_pos >= m_text.size())
                return fail("truncated JSON escape");

            const auto escape { m_text[m_pos++] };

            switch (escape)
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
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
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
                case 'u':
                {
                    std::uint32_t code { 0 };

                    if (!parse_hex4(code))
                        return false;

                    if (code >= 0xD800U && code <= 0xDBFFU)
                    {
                        if (m_pos + 2 > m_text.size() ||
                            m_text[m_pos] != '\\' || m_text[m_pos + 1] != 'u')
                            return fail("lone JSON surrogate");

                        m_pos += 2;

                        std::uint32_t low { 0 };

                        if (!parse_hex4(low))
                            return false;

                        if (low < 0xDC00U || low > 0xDFFFU)
                            return fail("invalid JSON surrogate pair");

                        code = 0x10000U + ((code - 0xD800U) << 10U) +
                               (low - 0xDC00U);
                    }
                    else if (code >= 0xDC00U && code <= 0xDFFFU)
                        return fail("lone JSON surrogate");

                    append_utf8(out, code);
                    break;
                }
                default:
                    return fail("unknown JSON escape");
            }
        }

        return fail("unterminated JSON string");
    }

    bool
    parse_hex4(std::uint32_t& out)
    {
        if (m_pos + 4 > m_text.size())
            return fail("truncated JSON unicode escape");

        std::uint32_t value { 0 };

        for (int i { 0 }; i < 4; ++i)
        {
            const auto ch { static_cast<unsigned char>(m_text[m_pos++]) };
            std::uint32_t digit { 0 };

            if (ch >= '0' && ch <= '9')
                digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                digit = ch - 'a' + 10U;
            else if (ch >= 'A' && ch <= 'F')
                digit = ch - 'A' + 10U;
            else
                return fail("invalid JSON unicode escape");

            value = (value << 4U) | digit;
        }

        out = value;
        return true;
    }

    static void
    append_utf8(std::string& out, std::uint32_t code)
    {
        if (code <= 0x7FU)
            out.push_back(static_cast<char>(code));
        else if (code <= 0x7FFU)
        {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
        else if (code <= 0xFFFFU)
        {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }

    bool
    parse_array(json_value& out, std::size_t depth)
    {
        ++m_pos;
        out.kind = json_value::kind_t::array;
        out.items.clear();
        skip_space();

        if (m_pos < m_text.size() && m_text[m_pos] == ']')
        {
            ++m_pos;
            return true;
        }

        while (true)
        {
            if (out.items.size() >= max_json_items)
                return fail("JSON array is too large");

            json_value item;
            skip_space();

            if (!parse_value(item, depth + 1))
                return false;

            out.items.push_back(std::move(item));
            skip_space();

            if (m_pos >= m_text.size())
                return fail("unterminated JSON array");

            if (m_text[m_pos] == ',')
            {
                ++m_pos;
                continue;
            }

            if (m_text[m_pos] == ']')
            {
                ++m_pos;
                return true;
            }

            return fail("expected ',' or ']' in JSON array");
        }
    }

    bool
    parse_object(json_value& out, std::size_t depth)
    {
        ++m_pos;
        out.kind = json_value::kind_t::object;
        out.members.clear();
        skip_space();

        if (m_pos < m_text.size() && m_text[m_pos] == '}')
        {
            ++m_pos;
            return true;
        }

        while (true)
        {
            if (out.members.size() >= max_json_items)
                return fail("JSON object is too large");

            skip_space();

            if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                return fail("expected JSON object key");

            std::string key;
            if (!parse_string(key))
                return false;

            skip_space();

            if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                return fail("expected ':' in JSON object");

            ++m_pos;
            skip_space();

            json_value member;
            if (!parse_value(member, depth + 1))
                return false;

            out.members.emplace_back(std::move(key), std::move(member));
            skip_space();

            if (m_pos >= m_text.size())
                return fail("unterminated JSON object");

            if (m_text[m_pos] == ',')
            {
                ++m_pos;
                continue;
            }

            if (m_text[m_pos] == '}')
            {
                ++m_pos;
                return true;
            }

            return fail("expected ',' or '}' in JSON object");
        }
    }

    void
    skip_space()
    {
        while (m_pos < m_text.size())
        {
            const auto ch { m_text[m_pos] };

            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
                return;

            ++m_pos;
        }
    }

    bool
    fail(std::string message)
    {
        if (m_error.empty())
            m_error = std::move(message);

        return false;
    }

    std::string_view m_text;
    std::size_t m_pos { 0 };
    std::string m_error;
};

void
append_escaped(std::string& out, std::string_view text)
{
    out.push_back('"');

    for (const char raw : text)
    {
        const auto ch { static_cast<unsigned char>(raw) };

        switch (ch)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
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
                if (ch < 0x20U)
                {
                    char buffer[7] {};
                    std::snprintf(
                        buffer,
                        sizeof(buffer),
                        "\\u%04x",
                        static_cast<unsigned int>(ch)
                    );
                    out += buffer;
                }
                else
                    out.push_back(raw);
                break;
        }
    }

    out.push_back('"');
}

void
append_number(std::string& out, double number)
{
    if (!std::isfinite(number))
    {
        out += "null";
        return;
    }

    char buffer[64] {};
    std::snprintf(buffer, sizeof(buffer), "%.17g", number);
    out += buffer;
}

void
append_uint(std::string& out, std::uint64_t number)
{
    out += std::to_string(number);
}

void
append_int(std::string& out, std::int64_t number)
{
    out += std::to_string(number);
}

void
append_bool(std::string& out, bool flag)
{
    out += flag ? "true" : "false";
}

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
            return "audio";
    }
}

bool
clock_from_name(std::string_view name, clock_tag& out) noexcept
{
    if (name == "audio")
    {
        out = clock_tag::audio;
        return true;
    }

    if (name == "ui")
    {
        out = clock_tag::ui;
        return true;
    }

    if (name == "worker")
    {
        out = clock_tag::worker;
        return true;
    }

    return false;
}

const char*
entry_kind_name(replay::entry_kind kind) noexcept
{
    switch (kind)
    {
        case replay::entry_kind::init:
            return "init";
        case replay::entry_kind::set_input:
            return "set_input";
        case replay::entry_kind::step_region:
            return "step_region";
        case replay::entry_kind::mark:
            return "mark";
        default:
            return "mark";
    }
}

bool
entry_kind_from_name(std::string_view name, replay::entry_kind& out) noexcept
{
    if (name == "init")
    {
        out = replay::entry_kind::init;
        return true;
    }

    if (name == "set_input")
    {
        out = replay::entry_kind::set_input;
        return true;
    }

    if (name == "step_region")
    {
        out = replay::entry_kind::step_region;
        return true;
    }

    if (name == "mark")
    {
        out = replay::entry_kind::mark;
        return true;
    }

    return false;
}

bool
as_uint(const json_value& node, std::uint64_t limit, std::uint64_t& out)
{
    if (!node.is_number() || !std::isfinite(node.number) || node.number < 0.0)
        return false;

    if (node.number > static_cast<double>(limit))
        return false;

    if (std::trunc(node.number) != node.number)
        return false;

    out = static_cast<std::uint64_t>(node.number);
    return true;
}

bool
as_int(
    const json_value& node,
    std::int64_t min,
    std::int64_t max,
    std::int64_t& out
)
{
    if (!node.is_number() || !std::isfinite(node.number))
        return false;

    if (node.number < static_cast<double>(min) ||
        node.number > static_cast<double>(max))
        return false;

    if (std::trunc(node.number) != node.number)
        return false;

    out = static_cast<std::int64_t>(node.number);
    return true;
}

bool
as_bool(const json_value& node, bool& out)
{
    if (!node.is_boolean())
        return false;

    out = node.boolean;
    return true;
}

bool
as_string(const json_value& node, std::string& out)
{
    if (!node.is_string())
        return false;

    if (node.text.size() > max_note_length)
        return false;

    out = node.text;
    return true;
}

bool
parse_json_text(const std::string& text, json_value& out, std::string& error)
{
    json_parser parser { text };
    return parser.parse(out, error);
}

std::string
make_error_json(std::string_view message)
{
    std::string out { "{\"ok\":false,\"error\":" };
    append_escaped(out, message);
    out += "}";
    return out;
}

constexpr char hex_digits[] = "0123456789abcdef";

std::string
hex_encode(const std::vector<std::byte>& bytes)
{
    std::string out;
    out.reserve(bytes.size() * 2);

    for (const auto raw : bytes)
    {
        const auto byte { std::to_integer<unsigned int>(raw) };
        out.push_back(
            hex_digits[static_cast<std::size_t>((byte >> 4U) & 0xFU)]
        );
        out.push_back(hex_digits[static_cast<std::size_t>(byte & 0xFU)]);
    }

    return out;
}

int
hex_digit(char ch) noexcept
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

bool
hex_decode(
    std::string_view text,
    std::vector<std::byte>& out,
    std::string& error
)
{
    if ((text.size() % 2U) != 0U)
    {
        error = "hex payload has an odd length";
        return false;
    }

    const auto bytes { text.size() / 2U };

    if (bytes > max_hex_bytes)
    {
        error = "hex payload is too large";
        return false;
    }

    out.clear();
    out.reserve(bytes);

    for (std::size_t i { 0 }; i < bytes; ++i)
    {
        const int high { hex_digit(text[i * 2U]) };
        const int low { hex_digit(text[i * 2U + 1U]) };

        if (high < 0 || low < 0)
        {
            error = "invalid hex payload";
            return false;
        }

        out.push_back(
            static_cast<std::byte>(
                static_cast<unsigned int>(high) << 4U |
                static_cast<unsigned int>(low)
            )
        );
    }

    return true;
}

template<typename T>
bool
user_bytes_as(const replay::encoded_value& value, T& out) noexcept
{
    const auto* bytes { std::get_if<replay::user_bytes>(&value.payload) };

    if (bytes == nullptr || bytes->bytes.size() != sizeof(T))
        return false;

    std::memcpy(&out, bytes->bytes.data(), sizeof(T));
    return true;
}

bool
encoded_to_json(
    const replay::encoded_value& value,
    std::string& out,
    std::string& error
)
{
    if (std::holds_alternative<std::monostate>(value.payload))
    {
        out += "{\"kind\":\"unit\"}";
        return true;
    }

    if (const auto* boolean { std::get_if<bool>(&value.payload) })
    {
        out += "{\"kind\":\"bool\",\"value\":";
        append_bool(out, *boolean);
        out += "}";
        return true;
    }

    if (const auto* integer { std::get_if<std::int64_t>(&value.payload) })
    {
        out += "{\"kind\":\"int\",\"value\":";
        append_int(out, *integer);
        out += "}";
        return true;
    }

    if (const auto* real { std::get_if<double>(&value.payload) })
    {
        out += "{\"kind\":\"double\",\"value\":";
        append_number(out, *real);
        out += "}";
        return true;
    }

    if (const auto* bytes { std::get_if<replay::user_bytes>(&value.payload) })
    {
        out += "{\"kind\":\"user\",\"type\":";
        append_uint(out, bytes->type_id);
        out += ",\"hex\":\"";
        out += hex_encode(bytes->bytes);
        out += "\"}";
        return true;
    }

    error = "unknown journal value kind";
    return false;
}

bool
json_to_encoded(
    const json_value& node,
    const replay::codec_registry& codecs,
    replay::encoded_value& out,
    std::string& error
)
{
    if (!node.is_object())
    {
        error = "journal value must be an object";
        return false;
    }

    const auto* kind { node.find("kind") };

    if (kind == nullptr || !kind->is_string())
    {
        error = "journal value is missing its kind";
        return false;
    }

    if (kind->text == "unit")
    {
        out.payload = std::monostate {};
        return true;
    }

    if (kind->text == "bool")
    {
        const auto* item { node.find("value") };
        bool flag { false };

        if (item == nullptr || !as_bool(*item, flag))
        {
            error = "journal bool value is invalid";
            return false;
        }

        out.payload = flag;
        return true;
    }

    if (kind->text == "int")
    {
        const auto* item { node.find("value") };
        std::int64_t number { 0 };

        if (item == nullptr || !as_int(
                                   *item,
                                   std::numeric_limits<std::int64_t>::min() / 2,
                                   std::numeric_limits<std::int64_t>::max() / 2,
                                   number
                               ))
        {
            error = "journal int value is invalid";
            return false;
        }

        out.payload = number;
        return true;
    }

    if (kind->text == "double")
    {
        const auto* item { node.find("value") };

        if (item == nullptr)
        {
            error = "journal double value is missing";
            return false;
        }

        if (item->is_number())
        {
            out.payload = item->number;
            return true;
        }

        if (item->is_string())
        {
            if (item->text == "nan")
            {
                out.payload = std::numeric_limits<double>::quiet_NaN();
                return true;
            }

            if (item->text == "inf")
            {
                out.payload = std::numeric_limits<double>::infinity();
                return true;
            }

            if (item->text == "-inf")
            {
                out.payload = -std::numeric_limits<double>::infinity();
                return true;
            }
        }

        error = "journal double value is invalid";
        return false;
    }

    if (kind->text == "user")
    {
        const auto* type { node.find("type") };
        const auto* hex { node.find("hex") };
        std::uint64_t type_id { 0 };

        if (type == nullptr ||
            !as_uint(*type, std::numeric_limits<std::uint32_t>::max(), type_id))
        {
            error = "journal user value is missing its type";
            return false;
        }

        if (hex == nullptr || !hex->is_string())
        {
            error = "journal user value is missing its bytes";
            return false;
        }

        replay::user_bytes bytes;
        bytes.type_id = static_cast<std::uint32_t>(type_id);

        if (!hex_decode(hex->text, bytes.bytes, error))
            return false;

        if (codecs.find(bytes.type_id) == nullptr)
        {
            error = "no replay codec is registered for journal type id " +
                    std::to_string(bytes.type_id);
            return false;
        }

        out.payload = std::move(bytes);
        return true;
    }

    error = "unknown journal value kind '" + kind->text + "'";
    return false;
}

jam::score
make_demo_score()
{
    jam::score score {};

    score.bpm = 120;

    auto& lead { score.patterns[0] };
    lead.length = 16;

    constexpr std::array<int, 16> lead_notes {
        60, 60, 64, 60, 67, 60, 64, 60, 60, 60, 67, 60, 72, 60, 67, 64,
    };
    constexpr std::array<int, 16> lead_velocity {
        96, 0, 80, 0, 88, 0, 80, 0, 96, 0, 88, 0, 104, 0, 80, 72,
    };

    for (std::size_t i { 0 }; i < lead_notes.size(); ++i)
        lead.steps[i] = jam::step {
            .note = static_cast<std::int8_t>(lead_notes[i]),
            .velocity = static_cast<std::uint8_t>(lead_velocity[i]),
            .gate = lead_velocity[i] > 0,
        };

    auto& bass { score.patterns[1] };
    bass.length = 16;

    constexpr std::array<int, 16> bass_notes {
        36, 0, 0, 0, 36, 0, 0, 0, 43, 0, 0, 0, 41, 0, 0, 0,
    };

    for (std::size_t i { 0 }; i < bass_notes.size(); ++i)
        bass.steps[i] = jam::step {
            .note = static_cast<std::int8_t>(bass_notes[i]),
            .velocity = static_cast<std::uint8_t>(bass_notes[i] > 0 ? 110 : 0),
            .gate = bass_notes[i] > 0,
        };

    auto& arp { score.patterns[2] };
    arp.length = 16;

    constexpr std::array<int, 16> arp_notes {
        72, 76, 79, 84, 79, 76, 72, 76, 74, 77, 81, 86, 81, 77, 74, 77,
    };

    for (std::size_t i { 0 }; i < arp_notes.size(); ++i)
        arp.steps[i] = jam::step {
            .note = static_cast<std::int8_t>(arp_notes[i]),
            .velocity = static_cast<std::uint8_t>(i % 2 == 0 ? 66 : 46),
            .gate = true,
        };

    auto& perc { score.patterns[3] };
    perc.length = 16;

    constexpr std::array<int, 16> perc_notes {
        84, 0, 0, 88, 0, 0, 84, 0, 86, 0, 0, 88, 0, 84, 0, 0,
    };

    for (std::size_t i { 0 }; i < perc_notes.size(); ++i)
        perc.steps[i] = jam::step {
            .note = static_cast<std::int8_t>(perc_notes[i]),
            .velocity = static_cast<std::uint8_t>(perc_notes[i] > 0 ? 52 : 0),
            .gate = perc_notes[i] > 0,
        };

    score.tracks[0] =
        jam::track { .pattern = 0, .transpose = 0, .gain = 210, .mute = false };
    score.tracks[1] =
        jam::track { .pattern = 1, .transpose = 0, .gain = 230, .mute = false };
    score.tracks[2] =
        jam::track { .pattern = 2, .transpose = 0, .gain = 140, .mute = false };
    score.tracks[3] =
        jam::track { .pattern = 3, .transpose = 0, .gain = 120, .mute = false };

    for (std::size_t i { 4 }; i < jam::track_count; ++i)
        score.tracks[i] = jam::track {
            .pattern = static_cast<std::uint8_t>(i),
            .transpose = 0,
            .gain = 160,
            .mute = true,
        };

    return score;
}

void
append_step(std::string& out, const jam::step& step)
{
    out.push_back('[');
    append_int(out, step.note);
    out.push_back(',');
    append_uint(out, step.velocity);
    out.push_back(',');
    append_bool(out, step.gate);
    out.push_back(']');
}

void
append_score(std::string& out, const jam::score& score)
{
    out += "{\"bpm\":";
    append_uint(out, score.bpm);
    out += ",\"patterns\":[";

    for (std::size_t p { 0 }; p < jam::pattern_count; ++p)
    {
        if (p != 0)
            out.push_back(',');

        const auto& pattern { score.patterns[p] };
        out += "{\"length\":";
        append_uint(out, pattern.length);
        out += ",\"steps\":[";

        for (std::size_t s { 0 }; s < jam::steps_per_pattern; ++s)
        {
            if (s != 0)
                out.push_back(',');

            append_step(out, pattern.steps[s]);
        }

        out += "]}";
    }

    out += "],\"tracks\":[";

    for (std::size_t t { 0 }; t < jam::track_count; ++t)
    {
        if (t != 0)
            out.push_back(',');

        const auto& track { score.tracks[t] };
        out += "{\"pattern\":";
        append_uint(out, track.pattern);
        out += ",\"transpose\":";
        append_int(out, track.transpose);
        out += ",\"gain\":";
        append_uint(out, track.gain);
        out += ",\"mute\":";
        append_bool(out, track.mute);
        out.push_back('}');
    }

    out += "]}";
}

bool
parse_step(const json_value& node, jam::step& out, std::string& error)
{
    std::int64_t note { 60 };
    std::int64_t velocity { 0 };
    bool gate { false };

    if (node.is_array() && node.items.size() == 3)
    {
        if (!as_int(node.items[0], 0, 127, note) ||
            !as_int(node.items[1], 0, 127, velocity) ||
            !as_bool(node.items[2], gate))
        {
            error = "score step arrays must be [note, velocity, gate]";
            return false;
        }
    }
    else if (node.is_object())
    {
        const auto* note_node { node.find("note") };
        const auto* velocity_node { node.find("velocity") };
        const auto* gate_node { node.find("gate") };

        if (note_node != nullptr && !as_int(*note_node, 0, 127, note))
        {
            error = "score step note is out of range";
            return false;
        }

        if (velocity_node != nullptr &&
            !as_int(*velocity_node, 0, 127, velocity))
        {
            error = "score step velocity is out of range";
            return false;
        }

        if (gate_node != nullptr && !as_bool(*gate_node, gate))
        {
            error = "score step gate must be a boolean";
            return false;
        }
    }
    else
    {
        error = "score step must be an array or an object";
        return false;
    }

    out = jam::step {
        .note = static_cast<std::int8_t>(note),
        .velocity = static_cast<std::uint8_t>(velocity),
        .gate = gate && velocity > 0,
    };

    return true;
}

bool
parse_pattern(const json_value& node, jam::pattern& out, std::string& error)
{
    if (!node.is_object())
    {
        error = "score pattern must be an object";
        return false;
    }

    const auto* length_node { node.find("length") };
    const auto* steps_node { node.find("steps") };

    if (length_node != nullptr)
    {
        std::int64_t length { 0 };

        if (!as_int(
                *length_node,
                0,
                static_cast<std::int64_t>(jam::steps_per_pattern),
                length
            ))
        {
            error = "score pattern length is out of range";
            return false;
        }

        out.length = static_cast<std::uint8_t>(length);
    }

    if (steps_node != nullptr)
    {
        if (!steps_node->is_array() ||
            steps_node->items.size() > jam::steps_per_pattern)
        {
            error = "score pattern steps must be an array of at most 32 steps";
            return false;
        }

        for (std::size_t i { 0 }; i < steps_node->items.size(); ++i)
            if (!parse_step(steps_node->items[i], out.steps[i], error))
                return false;
    }

    return true;
}

bool
parse_track(const json_value& node, jam::track& out, std::string& error)
{
    if (!node.is_object())
    {
        error = "score track must be an object";
        return false;
    }

    const auto* pattern_node { node.find("pattern") };
    const auto* transpose_node { node.find("transpose") };
    const auto* gain_node { node.find("gain") };
    const auto* mute_node { node.find("mute") };

    if (pattern_node != nullptr)
    {
        std::int64_t pattern { 0 };

        if (!as_int(
                *pattern_node,
                0,
                static_cast<std::int64_t>(jam::pattern_count - 1),
                pattern
            ))
        {
            error = "score track pattern is out of range";
            return false;
        }

        out.pattern = static_cast<std::uint8_t>(pattern);
    }

    if (transpose_node != nullptr)
    {
        std::int64_t transpose { 0 };

        if (!as_int(*transpose_node, -127, 127, transpose))
        {
            error = "score track transpose is out of range";
            return false;
        }

        out.transpose = static_cast<std::int8_t>(transpose);
    }

    if (gain_node != nullptr)
    {
        std::int64_t gain { 0 };

        if (!as_int(*gain_node, 0, 255, gain))
        {
            error = "score track gain is out of range";
            return false;
        }

        out.gain = static_cast<std::uint8_t>(gain);
    }

    if (mute_node != nullptr && !as_bool(*mute_node, out.mute))
    {
        error = "score track mute must be a boolean";
        return false;
    }

    return true;
}

bool
parse_score(const json_value& root, jam::score& out, std::string& error)
{
    if (!root.is_object())
    {
        error = "score must be a JSON object";
        return false;
    }

    jam::score score {};

    const auto* bpm_node { root.find("bpm") };

    if (bpm_node != nullptr)
    {
        std::int64_t bpm { 120 };

        if (!as_int(*bpm_node, 20, 255, bpm))
        {
            error = "score bpm is out of range";
            return false;
        }

        score.bpm = static_cast<std::uint8_t>(bpm);
    }

    const auto* patterns_node { root.find("patterns") };

    if (patterns_node == nullptr || !patterns_node->is_array())
    {
        error = "score is missing its patterns array";
        return false;
    }

    if (patterns_node->items.size() > jam::pattern_count)
    {
        error = "score has too many patterns";
        return false;
    }

    for (std::size_t i { 0 }; i < patterns_node->items.size(); ++i)
        if (!parse_pattern(patterns_node->items[i], score.patterns[i], error))
            return false;

    const auto* tracks_node { root.find("tracks") };

    if (tracks_node == nullptr || !tracks_node->is_array())
    {
        error = "score is missing its tracks array";
        return false;
    }

    if (tracks_node->items.size() > jam::track_count)
    {
        error = "score has too many tracks";
        return false;
    }

    for (std::size_t i { 0 }; i < tracks_node->items.size(); ++i)
        if (!parse_track(tracks_node->items[i], score.tracks[i], error))
            return false;

    out = score;
    return true;
}

bool
patch_op_from_name(std::string_view name, jam::patch_op& out) noexcept
{
    if (name == "set_step")
    {
        out = jam::patch_op::set_step;
        return true;
    }

    if (name == "clear_step")
    {
        out = jam::patch_op::clear_step;
        return true;
    }

    if (name == "transpose")
    {
        out = jam::patch_op::transpose;
        return true;
    }

    if (name == "rotate")
    {
        out = jam::patch_op::rotate;
        return true;
    }

    if (name == "mask")
    {
        out = jam::patch_op::mask;
        return true;
    }

    if (name == "density")
    {
        out = jam::patch_op::density;
        return true;
    }

    if (name == "stack")
    {
        out = jam::patch_op::stack;
        return true;
    }

    if (name == "sequence")
    {
        out = jam::patch_op::sequence;
        return true;
    }

    return false;
}

bool
parse_patch_op(const json_value& node, jam::patch_step& out, std::string& error)
{
    if (!node.is_object())
    {
        error = "patch operation must be an object";
        return false;
    }

    const auto* op_node { node.find("op") };

    if (op_node == nullptr || !op_node->is_string() ||
        !patch_op_from_name(op_node->text, out.op))
    {
        error = "patch operation has an unknown op";
        return false;
    }

    const auto read = [&node](
                          const char* name,
                          std::int64_t min,
                          std::int64_t max,
                          std::int64_t fallback,
                          std::int64_t& value
                      ) -> bool {
        const auto* field { node.find(name) };

        if (field == nullptr)
        {
            value = fallback;
            return true;
        }

        return as_int(*field, min, max, value);
    };

    std::int64_t a { 0 };
    std::int64_t b { 0 };
    std::int64_t c { 0 };
    std::int64_t d { 0 };

    if (!read("a", 0, 255, 0, a) || !read("b", 0, 255, 0, b) ||
        !read("c", 0, 255, 0, c) || !read("d", -32768, 32767, 0, d))
    {
        error = "patch operation field is out of range";
        return false;
    }

    const auto pattern_limit {
        static_cast<std::int64_t>(jam::pattern_count - 1)
    };
    const auto step_limit {
        static_cast<std::int64_t>(jam::steps_per_pattern - 1)
    };

    switch (out.op)
    {
        case jam::patch_op::set_step:
            if (a > pattern_limit || b > step_limit || c > 127 || d < 0 ||
                d > 127)
            {
                error = "set_step indices or values are out of range";
                return false;
            }
            break;

        case jam::patch_op::clear_step:
            if (a > pattern_limit || b > step_limit)
            {
                error = "clear_step indices are out of range";
                return false;
            }
            break;

        case jam::patch_op::transpose:
            if (a > pattern_limit || d < -127 || d > 127)
            {
                error = "transpose indices or value are out of range";
                return false;
            }
            break;

        case jam::patch_op::rotate:
            if (a > pattern_limit)
            {
                error = "rotate pattern index is out of range";
                return false;
            }
            break;

        case jam::patch_op::mask:
            if (a > pattern_limit || b > 255)
            {
                error = "mask indices are out of range";
                return false;
            }
            break;

        case jam::patch_op::density:
            if (a > pattern_limit || c == 0 || c > 32)
            {
                error = "density pattern or period is out of range";
                return false;
            }
            break;

        case jam::patch_op::stack:
        case jam::patch_op::sequence:
            if (a > pattern_limit || b > pattern_limit)
            {
                error = "stack/sequence pattern index is out of range";
                return false;
            }
            break;

        default:
            error = "patch operation is not supported";
            return false;
    }

    out.a = static_cast<std::uint8_t>(a);
    out.b = static_cast<std::uint8_t>(b);
    out.c = static_cast<std::uint8_t>(c);
    out.d = static_cast<std::int16_t>(d);

    return true;
}

bool
parse_patch(const json_value& root, jam::score_patch& out, std::string& error)
{
    const json_value* ops { nullptr };

    if (root.is_array())
        ops = &root;
    else if (root.is_object())
        ops = root.find("ops");

    if (ops == nullptr || !ops->is_array())
    {
        error = "score patch must be an array or an object with an ops array";
        return false;
    }

    if (ops->items.size() > max_branch_batch)
    {
        error = "score patch has too many operations";
        return false;
    }

    jam::score_patch patch {};

    for (std::size_t i { 0 }; i < ops->items.size(); ++i)
        if (!parse_patch_op(ops->items[i], patch.ops[i], error))
            return false;

    patch.count = static_cast<std::uint8_t>(ops->items.size());
    out = patch;
    return true;
}

void
append_patch(std::string& out, const jam::score_patch& patch)
{
    out += "{\"ops\":[";

    const auto count {
        std::min<std::size_t>(patch.count, jam::patch_capacity)
    };

    for (std::size_t i { 0 }; i < count; ++i)
    {
        if (i != 0)
            out.push_back(',');

        const auto& op { patch.ops[i] };
        out += "{\"a\":";
        append_uint(out, op.a);
        out += ",\"b\":";
        append_uint(out, op.b);
        out += ",\"c\":";
        append_uint(out, op.c);
        out += ",\"d\":";
        append_int(out, op.d);
        out += ",\"op\":";

        switch (op.op)
        {
            case jam::patch_op::set_step:
                out += "\"set_step\"";
                break;
            case jam::patch_op::clear_step:
                out += "\"clear_step\"";
                break;
            case jam::patch_op::transpose:
                out += "\"transpose\"";
                break;
            case jam::patch_op::rotate:
                out += "\"rotate\"";
                break;
            case jam::patch_op::mask:
                out += "\"mask\"";
                break;
            case jam::patch_op::density:
                out += "\"density\"";
                break;
            case jam::patch_op::stack:
                out += "\"stack\"";
                break;
            case jam::patch_op::sequence:
                out += "\"sequence\"";
                break;
            default:
                out += "\"clear_step\"";
                break;
        }

        out.push_back('}');
    }

    out += "]}";
}

std::string
scenario_to_json(const replay::scenario& scenario, const jam::score& initial)
{
    std::string out;
    out.reserve(1024 + scenario.entries.size() * 96);

    out += "{\"version\":\"clockr.replay.v1\",\"run\":";
    append_uint(out, scenario.run.value);
    out += ",\"branch\":";
    append_uint(out, scenario.branch.value);
    out += ",\"parent\":";

    if (scenario.parent)
        append_uint(out, scenario.parent->value);
    else
        out += "null";

    out += ",\"baseEntry\":";
    append_uint(out, scenario.base_entry.value);
    out += ",\"fidelity\":{\"complete\":";
    append_bool(out, scenario.fidelity.complete);
    out += ",\"missing\":";
    append_uint(out, scenario.fidelity.missing_entries);
    out += ",\"reason\":";
    append_escaped(out, scenario.fidelity.reason);
    out += "},\"initialScore\":";
    append_score(out, initial);
    out += ",\"entries\":[";

    for (std::size_t i { 0 }; i < scenario.entries.size(); ++i)
    {
        if (i != 0)
            out.push_back(',');

        const auto& entry { scenario.entries[i] };
        out += "{\"id\":";
        append_uint(out, entry.id.value);
        out += ",\"kind\":\"";
        out += entry_kind_name(entry.kind);
        out += "\",\"clock\":\"";
        out += clock_name(entry.clock);
        out += "\",\"epoch\":";
        append_uint(out, entry.epoch_group);
        out += ",\"port\":";
        append_escaped(out, entry.port);
        out += ",\"value\":";

        std::string value_error;
        if (!encoded_to_json(entry.value, out, value_error))
            out += "null";

        out += ",\"event\":";
        append_bool(out, entry.is_event);
        out += ",\"note\":";
        append_escaped(out, entry.note);
        out += ",\"threw\":";
        append_bool(out, entry.threw);
        out.push_back('}');
    }

    out += "]}";
    return out;
}

bool
parse_scenario_entry(
    const json_value& node,
    replay::scenario_entry& out,
    const replay::codec_registry& codecs,
    std::string& error
)
{
    if (!node.is_object())
    {
        error = "scenario entry must be an object";
        return false;
    }

    const auto* id_node { node.find("id") };
    const auto* kind_node { node.find("kind") };
    const auto* clock_node { node.find("clock") };
    const auto* epoch_node { node.find("epoch") };
    const auto* port_node { node.find("port") };
    const auto* value_node { node.find("value") };
    const auto* event_node { node.find("event") };
    const auto* note_node { node.find("note") };
    const auto* threw_node { node.find("threw") };

    std::uint64_t id { 0 };

    if (id_node == nullptr ||
        !as_uint(*id_node, std::numeric_limits<std::uint64_t>::max(), id) ||
        id == 0)
    {
        error = "scenario entry id is invalid";
        return false;
    }

    if (kind_node == nullptr || !kind_node->is_string() ||
        !entry_kind_from_name(kind_node->text, out.kind))
    {
        error = "scenario entry kind is invalid";
        return false;
    }

    if (clock_node == nullptr || !clock_node->is_string() ||
        !clock_from_name(clock_node->text, out.clock))
    {
        error = "scenario entry clock is invalid";
        return false;
    }

    if (epoch_node != nullptr)
    {
        std::uint64_t epoch { 0 };

        if (!as_uint(
                *epoch_node, std::numeric_limits<std::uint64_t>::max(), epoch
            ))
        {
            error = "scenario entry epoch is invalid";
            return false;
        }

        out.epoch_group = epoch;
    }

    if (port_node != nullptr && !port_node->is_null() &&
        !as_string(*port_node, out.port))
    {
        error = "scenario entry port is invalid";
        return false;
    }

    if (event_node != nullptr && !as_bool(*event_node, out.is_event))
    {
        error = "scenario entry event flag is invalid";
        return false;
    }

    if (note_node != nullptr && !note_node->is_null() &&
        !as_string(*note_node, out.note))
    {
        error = "scenario entry note is invalid";
        return false;
    }

    if (threw_node != nullptr && !as_bool(*threw_node, out.threw))
    {
        error = "scenario entry threw flag is invalid";
        return false;
    }

    out.id = replay::entry_id { id };

    if (value_node != nullptr && !value_node->is_null())
    {
        if (!json_to_encoded(*value_node, codecs, out.value, error))
            return false;
    }
    else if (out.kind == replay::entry_kind::set_input)
    {
        error = "scenario set_input entry is missing its value";
        return false;
    }

    if (out.kind == replay::entry_kind::set_input && out.port.empty())
    {
        error = "scenario set_input entry is missing its port";
        return false;
    }

    return true;
}

bool
json_to_scenario(
    const json_value& root,
    replay::scenario& out,
    jam::score& initial,
    const replay::codec_registry& codecs,
    std::string& error
)
{
    if (!root.is_object())
    {
        error = "scenario must be a JSON object";
        return false;
    }

    const auto* version { root.find("version") };

    if (version == nullptr || !version->is_string() ||
        version->text != "clockr.replay.v1")
    {
        error = "scenario version must be clockr.replay.v1";
        return false;
    }

    const auto* run_node { root.find("run") };
    const auto* branch_node { root.find("branch") };
    const auto* parent_node { root.find("parent") };
    const auto* base_node { root.find("baseEntry") };
    const auto* entries_node { root.find("entries") };

    std::uint64_t run { 0 };
    std::uint64_t branch { 0 };
    std::uint64_t base { 0 };

    if (run_node == nullptr ||
        !as_uint(*run_node, std::numeric_limits<std::uint64_t>::max(), run))
    {
        error = "scenario run id is invalid";
        return false;
    }

    if (branch_node == nullptr ||
        !as_uint(
            *branch_node, std::numeric_limits<std::uint64_t>::max(), branch
        ))
    {
        error = "scenario branch id is invalid";
        return false;
    }

    if (base_node != nullptr &&
        !as_uint(*base_node, std::numeric_limits<std::uint64_t>::max(), base))
    {
        error = "scenario base entry is invalid";
        return false;
    }

    std::optional<replay::run_id> parent;

    if (parent_node != nullptr && !parent_node->is_null())
    {
        std::uint64_t parent_id { 0 };

        if (!as_uint(
                *parent_node,
                std::numeric_limits<std::uint64_t>::max(),
                parent_id
            ))
        {
            error = "scenario parent id is invalid";
            return false;
        }

        parent = replay::run_id { parent_id };
    }

    replay::scenario scenario;
    scenario.run = replay::run_id { run };
    scenario.branch = replay::branch_id { branch };
    scenario.parent = parent;
    scenario.base_entry = replay::entry_id { base };

    const auto* fidelity_node { root.find("fidelity") };

    if (fidelity_node != nullptr)
    {
        if (!fidelity_node->is_object())
        {
            error = "scenario fidelity must be an object";
            return false;
        }

        const auto* complete { fidelity_node->find("complete") };
        const auto* missing { fidelity_node->find("missing") };
        const auto* reason { fidelity_node->find("reason") };

        if (complete != nullptr &&
            !as_bool(*complete, scenario.fidelity.complete))
        {
            error = "scenario fidelity complete flag is invalid";
            return false;
        }

        if (missing != nullptr && !as_uint(
                                      *missing,
                                      std::numeric_limits<std::uint64_t>::max(),
                                      scenario.fidelity.missing_entries
                                  ))
        {
            error = "scenario fidelity missing count is invalid";
            return false;
        }

        if (reason != nullptr && !as_string(*reason, scenario.fidelity.reason))
        {
            error = "scenario fidelity reason is invalid";
            return false;
        }
    }

    const auto* score_node { root.find("initialScore") };

    if (score_node != nullptr && !parse_score(*score_node, initial, error))
        return false;

    if (entries_node == nullptr || !entries_node->is_array())
    {
        error = "scenario is missing its entries array";
        return false;
    }

    if (entries_node->items.size() > max_scenario_entries)
    {
        error = "scenario has too many entries";
        return false;
    }

    scenario.entries.reserve(entries_node->items.size());
    std::uint64_t previous_id { 0 };

    for (const auto& item : entries_node->items)
    {
        replay::scenario_entry entry;

        if (!parse_scenario_entry(item, entry, codecs, error))
            return false;

        if (entry.id.value <= previous_id)
        {
            error = "scenario entries are not ordered by increasing id";
            return false;
        }

        previous_id = entry.id.value;
        scenario.entries.push_back(std::move(entry));
    }

    out = std::move(scenario);
    return true;
}

void
append_events(std::string& out, const jam::event_batch& batch)
{
    const auto count {
        std::min<std::size_t>(batch.count, jam::event_capacity)
    };

    out += "[";

    for (std::size_t i { 0 }; i < count; ++i)
    {
        if (i != 0)
            out.push_back(',');

        const auto& event { batch.events[i] };
        out += "{\"note\":";
        append_int(out, event.note);
        out += ",\"velocity\":";
        append_uint(out, event.velocity);
        out += ",\"offset\":";
        append_int(out, event.offset);
        out += ",\"track\":";
        append_uint(out, event.track);
        out += ",\"on\":";
        append_bool(out, event.on);
        out.push_back('}');
    }

    out += "]";
}

void
append_voices(std::string& out, const jam::voice_state& voices)
{
    out += "[";

    bool first { true };

    for (const auto& voice : voices.slots)
    {
        if (!voice.active)
            continue;

        if (!first)
            out.push_back(',');

        first = false;
        out += "{\"note\":";
        append_int(out, voice.note);
        out += ",\"velocity\":";
        append_uint(out, voice.velocity);
        out += ",\"track\":";
        append_uint(out, voice.track);
        out += ",\"serial\":";
        append_uint(out, voice.serial);
        out.push_back('}');
    }

    out += "]";
}

void
append_event_summary(std::string& out, const jam::event_batch& batch)
{
    out += "\"eventCount\":";
    append_uint(out, std::min<std::size_t>(batch.count, jam::event_capacity));
    out += ",\"dropped\":";
    append_uint(out, batch.dropped);
    out += ",\"events\":";
    append_events(out, batch);
}

double
read_double(const kernel::program& program, std::string_view name)
{
    const auto encoded { program.get(name) };

    if (const auto* number { std::get_if<double>(&encoded) })
        return *number;

    return std::numeric_limits<double>::quiet_NaN();
}

const jam::event_batch*
read_events(const kernel::program& program, jam::event_batch& storage)
{
    try
    {
        storage =
            program.get_or<jam::event_batch>("events", jam::event_batch {});
        return &storage;
    }
    catch (...)
    {
        return nullptr;
    }
}

const jam::voice_state*
read_voices(const kernel::program& program, jam::voice_state& storage)
{
    try
    {
        storage =
            program.get_or<jam::voice_state>("voices", jam::voice_state {});
        return &storage;
    }
    catch (...)
    {
        return nullptr;
    }
}

constexpr std::uint64_t kFNVOffset { 0xcbf29ce484222325ULL };
constexpr std::uint64_t kFNVPrime { 0x100000001b3ULL };

void
hash_uint64(std::uint64_t& hash, std::uint64_t value) noexcept
{
    for (std::uint32_t byte { 0 }; byte < 8; ++byte)
    {
        hash ^= (value >> (byte * 8U)) & 0xffU;
        hash *= kFNVPrime;
    }
}

void
hash_bytes(
    std::uint64_t& hash,
    const std::byte* data,
    std::size_t count
) noexcept
{
    for (std::size_t i { 0 }; i < count; ++i)
    {
        hash ^=
            static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[i]));
        hash *= kFNVPrime;
    }
}

void
hash_encoded(std::uint64_t& hash, const replay::encoded_value& value) noexcept
{
    hash_uint64(hash, static_cast<std::uint64_t>(value.payload.index()));

    if (const auto* boolean { std::get_if<bool>(&value.payload) })
    {
        hash_uint64(hash, *boolean ? 1U : 0U);
        return;
    }

    if (const auto* integer { std::get_if<std::int64_t>(&value.payload) })
    {
        hash_uint64(hash, std::bit_cast<std::uint64_t>(*integer));
        return;
    }

    if (const auto* real { std::get_if<double>(&value.payload) })
    {
        hash_uint64(hash, std::bit_cast<std::uint64_t>(*real));
        return;
    }

    if (const auto* user { std::get_if<replay::user_bytes>(&value.payload) })
    {
        hash_uint64(hash, static_cast<std::uint64_t>(user->type_id));
        hash_uint64(hash, static_cast<std::uint64_t>(user->bytes.size()));
        hash_bytes(hash, user->bytes.data(), user->bytes.size());
        return;
    }
}

std::uint64_t
run_digest(const replay::run& run) noexcept
{
    std::uint64_t hash { kFNVOffset };

    hash_uint64(hash, static_cast<std::uint64_t>(run.steps.size()));

    for (const auto& step : run.steps)
    {
        hash_uint64(
            hash,
            static_cast<std::uint64_t>(kernel::clock_tag_index(step.step.clock))
        );
        hash_uint64(hash, step.step.ordinal);
        hash_uint64(hash, static_cast<std::uint64_t>(step.outputs.size()));

        for (const auto& output : step.outputs)
            hash_encoded(hash, output);
    }

    return hash;
}

std::string
digest_hex(std::uint64_t digest)
{
    std::string out { "0x" };

    for (int shift { 60 }; shift >= 0; shift -= 4)
        out.push_back(
            hex_digits[static_cast<std::size_t>(
                (digest >> static_cast<unsigned int>(shift)) & 0xFU
            )]
        );

    return out;
}

double
encoded_double(const replay::encoded_value& value, double fallback)
{
    if (const auto* real { std::get_if<double>(&value.payload) })
        return *real;

    return fallback;
}

struct tick_outputs
{
    double playhead { std::numeric_limits<double>::quiet_NaN() };
    double mix { std::numeric_limits<double>::quiet_NaN() };
    double playhead_ui { std::numeric_limits<double>::quiet_NaN() };
    jam::event_batch events {};
    jam::voice_state voices {};
    bool has_events { false };
    bool has_voices { false };
};

tick_outputs
read_tick_outputs(const kernel::program& program)
{
    tick_outputs out;
    out.playhead = read_double(program, "playhead");
    out.mix = read_double(program, "mix");
    out.playhead_ui = read_double(program, "playhead_ui");
    out.has_events = read_events(program, out.events) != nullptr;
    out.has_voices = read_voices(program, out.voices) != nullptr;
    return out;
}

tick_outputs
decode_tick_outputs(const replay::step_record& record)
{
    tick_outputs out;

    if (record.outputs.size() > output_playhead)
        out.playhead =
            encoded_double(record.outputs[output_playhead], out.playhead);

    if (record.outputs.size() > output_mix)
        out.mix = encoded_double(record.outputs[output_mix], out.mix);

    if (record.outputs.size() > output_playhead_ui)
        out.playhead_ui =
            encoded_double(record.outputs[output_playhead_ui], out.playhead_ui);

    if (record.outputs.size() > output_events)
        out.has_events =
            user_bytes_as(record.outputs[output_events], out.events);

    if (record.outputs.size() > output_voices)
        out.has_voices =
            user_bytes_as(record.outputs[output_voices], out.voices);

    return out;
}

void
append_tick(
    std::string& out,
    std::uint64_t index,
    std::int64_t start,
    std::int64_t frames,
    double rate,
    const tick_outputs& outputs
)
{
    out += "{\"index\":";
    append_uint(out, index);
    out += ",\"start\":";
    append_int(out, start);
    out += ",\"frames\":";
    append_int(out, frames);
    out += ",\"rate\":";
    append_number(out, rate);
    out += ",\"playhead\":";
    append_number(out, outputs.playhead);
    out += ",\"mix\":";
    append_number(out, outputs.mix);
    out += ",\"playheadUi\":";
    append_number(out, outputs.playhead_ui);

    if (outputs.has_events)
    {
        out.push_back(',');
        append_event_summary(out, outputs.events);
    }
    else
        out += ",\"eventCount\":0,\"dropped\":0,\"events\":[]";

    out += ",\"voices\":";
    append_voices(out, outputs.voices);
    out.push_back('}');
}

std::string
audio_ticks_json(
    const replay::scenario& scenario,
    const replay::run& run,
    std::size_t max_ticks,
    bool& truncated
)
{
    std::string out { "[" };
    truncated = false;

    std::size_t step_index { 0 };
    std::size_t emitted { 0 };
    bool first { true };
    daw::BlockDesc block {};
    bool have_block { false };

    for (const auto& entry : scenario.entries)
    {
        if (entry.kind == replay::entry_kind::set_input &&
            entry.port == "block")
        {
            daw::BlockDesc decoded {};

            if (user_bytes_as(entry.value, decoded))
            {
                block = decoded;
                have_block = true;
            }

            continue;
        }

        if (entry.kind != replay::entry_kind::step_region)
            continue;

        const auto* record =
            step_index < run.steps.size() ? &run.steps[step_index] : nullptr;

        ++step_index;

        if (entry.clock != clock_tag::audio || record == nullptr)
            continue;

        if (emitted >= max_ticks)
        {
            truncated = true;
            continue;
        }

        const auto outputs { decode_tick_outputs(*record) };
        const auto start { have_block ? block.start_sample
                                      : static_cast<std::int64_t>(emitted) *
                                            frames_per_block };
        const auto frames { have_block ? block.n_frames : frames_per_block };
        const auto rate { have_block ? block.sample_rate : sample_rate };

        if (!first)
            out.push_back(',');

        first = false;
        append_tick(
            out,
            static_cast<std::uint64_t>(emitted),
            start,
            frames,
            rate,
            outputs
        );
        ++emitted;
    }

    out += "]";
    return out;
}

using recipe_type = decltype(jam::net::app(jam::score {}));

kernel::program_options
make_program_options()
{
    kernel::program_options options;
    options.region_pool_bytes = std::size_t { 2 } << 20;
    return options;
}

struct intents
{
    bool playing { false };
    double bpm { 120.0 };
    double loop_start { 0.0 };
    double loop_end { loop_end_default };
    bool loop_enabled { true };
    double master { master_default };
};

class jam_shell
{
public:
    jam_shell() :
        m_initial { make_demo_score() },
        m_score { m_initial },
        m_recipe { jam::net::app(m_initial) },
        m_program {
            std::make_unique<kernel::program>(m_recipe, make_program_options())
        }
    {
        register_codecs();
        start_journal();
    }

    jam_shell(const jam_shell&) = delete;
    jam_shell&
    operator=(const jam_shell&) = delete;

    ~jam_shell() = default;

    std::string
    create_score() const
    {
        return score_json(make_demo_score());
    }

    std::string
    default_score_json() const
    {
        return create_score();
    }

    std::string
    init(double bpm)
    {
        if (std::isfinite(bpm))
            m_intents.bpm = clamp_bpm(bpm);

        if (m_initialized)
            apply_intents();
        else
            initialize();

        return state_json();
    }

    std::string
    set_intents(
        double playing,
        double bpm,
        double loop_start,
        double loop_end,
        double loop_enabled,
        double master
    )
    {
        m_intents.playing = playing != 0.0;

        if (std::isfinite(bpm))
            m_intents.bpm = clamp_bpm(bpm);

        if (std::isfinite(loop_start) && std::isfinite(loop_end))
        {
            const auto start { std::max(0.0, loop_start) };
            const auto end { std::max(start + 0.25, loop_end) };
            m_intents.loop_start = start;
            m_intents.loop_end = end;
        }

        m_intents.loop_enabled = loop_enabled != 0.0;

        if (std::isfinite(master))
            m_intents.master = std::clamp(master, 0.0, 1.0);

        if (m_initialized)
            apply_intents();
        else
            initialize();

        return state_json();
    }

    std::string
    set_score_json(const std::string& text)
    {
        json_value root;
        std::string error;

        if (!parse_json_text(text, root, error))
            return make_error_json(error);

        jam::score score {};
        if (!parse_score(root, score, error))
            return make_error_json(error);

        restart_with_score(score, true);
        return state_json();
    }

    std::string
    set_patch_json(const std::string& text)
    {
        json_value root;
        std::string error;

        if (!parse_json_text(text, root, error))
            return make_error_json(error);

        jam::score_patch patch {};
        if (!parse_patch(root, patch, error))
            return make_error_json(error);

        return submit_patch(patch);
    }

    std::string
    set_step(
        double pattern,
        double step,
        double note,
        double velocity,
        double gate
    )
    {
        std::int64_t p { 0 };
        std::int64_t s { 0 };
        std::int64_t n { 0 };
        std::int64_t v { 0 };

        const auto integer = [](double number,
                                std::int64_t min,
                                std::int64_t max,
                                std::int64_t& out) -> bool {
            if (!std::isfinite(number) || number < static_cast<double>(min) ||
                number > static_cast<double>(max) ||
                std::trunc(number) != number)
                return false;

            out = static_cast<std::int64_t>(number);
            return true;
        };

        if (!integer(
                pattern, 0, static_cast<std::int64_t>(jam::pattern_count - 1), p
            ) ||
            !integer(
                step,
                0,
                static_cast<std::int64_t>(jam::steps_per_pattern - 1),
                s
            ) ||
            !integer(note, 0, 127, n) || !integer(velocity, 0, 127, v))
            return make_error_json("step edit parameters are out of range");

        const bool engaged { gate != 0.0 && v > 0 };

        jam::score_patch patch {};
        patch.count = 1;

        if (engaged)
            patch.ops[0] = jam::patch_step {
                .op = jam::patch_op::set_step,
                .a = static_cast<std::uint8_t>(p),
                .b = static_cast<std::uint8_t>(s),
                .c = static_cast<std::uint8_t>(n),
                .d = static_cast<std::int16_t>(v),
            };
        else
            patch.ops[0] = jam::patch_step {
                .op = jam::patch_op::clear_step,
                .a = static_cast<std::uint8_t>(p),
                .b = static_cast<std::uint8_t>(s),
            };

        return submit_patch(patch);
    }

    std::string
    step(double count)
    {
        if (!m_initialized)
            initialize();

        std::size_t blocks { 0 };

        if (std::isfinite(count) && count > 0.0)
        {
            const auto requested {
                std::min(count, static_cast<double>(max_step_blocks))
            };
            blocks = static_cast<std::size_t>(requested);
        }

        std::string ticks { "[" };
        bool first { true };

        for (std::size_t i { 0 }; i < blocks; ++i)
        {
            if (m_block_index > 0 && m_block_index % ui_every_blocks == 0)
            {
                m_session->begin_epoch(clock_tag::ui);
                m_session->step(clock_tag::ui);
                capture(clock_tag::ui);
            }

            m_session->begin_epoch(clock_tag::audio);
            m_session->set<daw::BlockDesc>(
                "block", m_next_frame, frames_per_block, sample_rate
            );
            m_session->step(clock_tag::audio);
            capture(clock_tag::audio);

            const auto outputs { read_tick_outputs(*m_program) };

            if (!first)
                ticks.push_back(',');

            first = false;
            append_tick(
                ticks,
                m_block_index,
                m_next_frame,
                frames_per_block,
                sample_rate,
                outputs
            );

            m_next_frame += frames_per_block;
            ++m_block_index;
        }

        ticks += "]";

        const auto outputs { read_tick_outputs(*m_program) };

        std::string out { "{\"ticks\":" };
        out += ticks;
        out += ",\"block\":{\"index\":";
        append_uint(out, m_block_index);
        out += ",\"start\":";
        append_int(out, m_next_frame);
        out += ",\"frames\":";
        append_int(out, frames_per_block);
        out += ",\"rate\":";
        append_number(out, sample_rate);
        out += "},\"playhead\":";
        append_number(out, outputs.playhead);
        out += ",\"playheadUi\":";
        append_number(out, outputs.playhead_ui);
        out += ",\"mix\":";
        append_number(out, outputs.mix);
        out += ",\"eventCount\":";
        append_uint(
            out,
            outputs.has_events ? std::min<std::size_t>(
                                     outputs.events.count, jam::event_capacity
                                 )
                               : 0
        );
        out += ",\"dropped\":";
        append_uint(out, outputs.has_events ? outputs.events.dropped : 0);
        out += ",\"events\":";
        if (outputs.has_events)
            append_events(out, outputs.events);
        else
            out += "[]";
        out += ",\"voices\":";
        append_voices(out, outputs.voices);
        out.push_back('}');
        return out;
    }

    std::string
    get_state_json() const
    {
        return state_json();
    }

    std::string
    export_scenario_json() const
    {
        if (m_imported_scenario)
            return scenario_to_json(*m_imported_scenario, m_imported_score);

        return scenario_to_json(recorded_scenario(), m_initial);
    }

    std::string
    import_scenario_json(const std::string& text)
    {
        json_value root;
        std::string error;

        if (!parse_json_text(text, root, error))
            return make_error_json(error);

        replay::scenario scenario;
        jam::score initial { m_initial };

        if (!json_to_scenario(root, scenario, initial, m_codecs, error))
            return make_error_json(error);

        replay::run run;

        try
        {
            run = replay_with_score(scenario, initial);
        }
        catch (const std::exception& exception)
        {
            return make_error_json(
                std::string { "scenario replay failed: " } + exception.what()
            );
        }
        catch (...)
        {
            return make_error_json("scenario replay failed");
        }

        if (!run.fidelity.complete || run.aborted)
        {
            auto message { std::string { "scenario is not replayable" } };

            if (!run.fidelity.reason.empty())
                message += ": " + run.fidelity.reason;

            return make_error_json(message);
        }

        m_branch_scenario.reset();
        m_branch_run.reset();
        m_branch_original_scenario.reset();
        m_branch_original_run.reset();
        m_imported_scenario = std::move(scenario);
        m_imported_score = initial;
        m_imported_run = std::move(run);

        std::string out { "{\"ok\":true,\"entries\":" };
        append_uint(out, m_imported_scenario->entries.size());
        out += ",\"digest\":\"";
        out += digest_hex(run_digest(*m_imported_run));
        out += "\",\"run\":";
        append_uint(out, m_imported_scenario->run.value);
        out += ",\"branch\":";
        append_uint(out, m_imported_scenario->branch.value);
        out += ",\"fidelity\":{\"complete\":";
        append_bool(out, m_imported_scenario->fidelity.complete);
        out += ",\"missing\":";
        append_uint(out, m_imported_scenario->fidelity.missing_entries);
        out += ",\"reason\":";
        append_escaped(out, m_imported_scenario->fidelity.reason);
        out += "}}";
        return out;
    }

    std::string
    replay_branch_json(const std::string& text)
    {
        json_value root;
        std::string error;

        if (!parse_json_text(text, root, error))
            return make_error_json(error);

        jam::score_patch patch {};
        std::optional<std::uint64_t> cut;

        json_value patch_only { root };

        if (root.is_object() && root.find("ops") != nullptr)
            patch_only = root;
        else
        {
            const auto* patch_node { root.is_object() ? root.find("patch")
                                                      : nullptr };

            if (patch_node == nullptr)
                return make_error_json("branch edit must contain a patch");

            patch_only = *patch_node;
        }

        if (!parse_patch(patch_only, patch, error))
            return make_error_json(error);

        if (root.is_object())
        {
            const auto* cut_node { root.find("cut") };

            if (cut_node != nullptr && !cut_node->is_null())
            {
                std::uint64_t value { 0 };

                if (!as_uint(
                        *cut_node,
                        std::numeric_limits<std::uint64_t>::max(),
                        value
                    ))
                    return make_error_json("branch cut entry is invalid");

                cut = value;
            }
        }

        const auto base_scenario { m_imported_scenario ? *m_imported_scenario
                                                       : recorded_scenario() };
        const auto base_score { m_imported_scenario ? m_imported_score
                                                    : m_initial };
        const replay::run& base_run { m_imported_scenario ? *m_imported_run
                                                          : m_live_run };

        const auto chosen_cut {
            cut.value_or(default_branch_cut(base_scenario))
        };

        if (chosen_cut == 0)
            return make_error_json(
                "scenario has no audio step to branch before"
            );

        const auto cut_exists { std::any_of(
            base_scenario.entries.begin(),
            base_scenario.entries.end(),
            [chosen_cut](const replay::scenario_entry& entry) {
                return entry.id.value == chosen_cut;
            }
        ) };

        if (!cut_exists)
            return make_error_json("branch cut entry is not in the scenario");

        replay::branch_edit edit;
        edit.cut = replay::entry_id { chosen_cut };

        replay::scenario_entry inserted_patch;
        inserted_patch.kind = replay::entry_kind::set_input;
        inserted_patch.clock = clock_tag::ui;
        inserted_patch.port = "patch";
        inserted_patch.is_event = true;
        inserted_patch.note = "branch edit";

        replay::encoded_value encoded;
        if (!encode_patch(patch, encoded, error))
            return make_error_json(error);

        inserted_patch.value = std::move(encoded);

        replay::scenario_entry inserted_tick;
        inserted_tick.kind = replay::entry_kind::step_region;
        inserted_tick.clock = clock_tag::ui;

        edit.inserted.push_back(std::move(inserted_patch));
        edit.inserted.push_back(std::move(inserted_tick));

        const auto branch_id { replay::branch_id { base_scenario.branch.value +
                                                   1 } };

        replay::run branch_run;

        try
        {
            branch_run = replay_branch_with_score(
                base_scenario, edit, branch_id, base_score
            );
        }
        catch (const std::exception& exception)
        {
            return make_error_json(
                std::string { "branch replay failed: " } + exception.what()
            );
        }
        catch (...)
        {
            return make_error_json("branch replay failed");
        }

        if (branch_run.aborted || !branch_run.fidelity.complete)
        {
            auto message { std::string { "branch replay is incomplete" } };

            if (!branch_run.fidelity.reason.empty())
                message += ": " + branch_run.fidelity.reason;

            return make_error_json(message);
        }

        const auto branched {
            replay::apply_branch(base_scenario, edit, branch_id)
        };
        const auto diff { replay::compare(base_run, branch_run) };

        m_branch_scenario = branched;
        m_branch_run = std::move(branch_run);
        m_branch_original_scenario = base_scenario;
        m_branch_original_run = base_run;

        std::string out { "{\"ok\":true,\"branch\":" };
        append_uint(out, branch_id.value);
        out += ",\"cut\":";
        append_uint(out, chosen_cut);
        out += ",\"digest\":\"";
        out += digest_hex(run_digest(*m_branch_run));
        out += "\",\"originalDigest\":\"";
        out += digest_hex(run_digest(*m_branch_original_run));
        out += "\",\"equal\":";
        append_bool(out, diff.equal);
        out += ",\"firstStep\":";
        append_uint(out, diff.first_step);
        out += ",\"outputMismatches\":";
        append_uint(out, diff.output_mismatches);
        out += ",\"traceMismatches\":";
        append_uint(out, diff.trace_mismatches);
        out += ",\"summary\":";
        append_escaped(out, diff.summary);
        out += ",\"patch\":";
        append_patch(out, patch);

        bool original_truncated { false };
        bool candidate_truncated { false };

        out += ",\"originalTicks\":";
        out += audio_ticks_json(
            *m_branch_original_scenario,
            *m_branch_original_run,
            max_branch_ticks,
            original_truncated
        );
        out += ",\"candidateTicks\":";
        out += audio_ticks_json(
            *m_branch_scenario,
            *m_branch_run,
            max_branch_ticks,
            candidate_truncated
        );
        out += ",\"truncated\":";
        append_bool(out, original_truncated || candidate_truncated);
        out.push_back('}');
        return out;
    }

    std::string
    compare_with_original_json() const
    {
        if (!m_branch_run || !m_branch_original_run)
            return make_error_json("no branch has been replayed yet");

        const auto diff {
            replay::compare(*m_branch_original_run, *m_branch_run)
        };

        std::string out { "{\"ok\":true,\"equal\":" };
        append_bool(out, diff.equal);
        out += ",\"firstStep\":";
        append_uint(out, diff.first_step);
        out += ",\"outputMismatches\":";
        append_uint(out, diff.output_mismatches);
        out += ",\"traceMismatches\":";
        append_uint(out, diff.trace_mismatches);
        out += ",\"summary\":";
        append_escaped(out, diff.summary);
        out += ",\"originalDigest\":\"";
        out += digest_hex(run_digest(*m_branch_original_run));
        out += "\",\"candidateDigest\":\"";
        out += digest_hex(run_digest(*m_branch_run));
        out += "\"}";
        return out;
    }

    std::string
    replay_block_json(double block_number) const
    {
        const replay::scenario& base_scenario {
            m_imported_scenario ? *m_imported_scenario : recorded_scenario()
        };
        const jam::score& base_score {
            m_imported_scenario ? m_imported_score : m_initial
        };

        std::vector<replay::entry_id> audio_steps;

        for (const auto& entry : base_scenario.entries)
            if (entry.kind == replay::entry_kind::step_region &&
                entry.clock == clock_tag::audio)
                audio_steps.push_back(entry.id);

        if (audio_steps.empty())
            return make_error_json("the journal has no audio block to replay");

        std::size_t index { 0 };

        if (std::isfinite(block_number) && block_number >= 0.0)
            index = static_cast<std::size_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(block_number),
                audio_steps.size() - 1
            ));

        const auto cut { audio_steps[index] };
        replay::scenario prefix { base_scenario };
        prefix.entries.erase(
            std::remove_if(
                prefix.entries.begin(),
                prefix.entries.end(),
                [cut](const replay::scenario_entry& entry) {
                    return entry.id > cut;
                }
            ),
            prefix.entries.end()
        );
        prefix.base_entry = cut;

        replay::run run;

        try
        {
            run = replay_with_score(prefix, base_score);
        }
        catch (const std::exception& exception)
        {
            return make_error_json(
                std::string { "prefix replay failed: " } + exception.what()
            );
        }
        catch (...)
        {
            return make_error_json("prefix replay failed");
        }

        if (run.aborted || !run.fidelity.complete)
        {
            auto message { std::string { "prefix is not replayable" } };

            if (!run.fidelity.reason.empty())
                message += ": " + run.fidelity.reason;

            return make_error_json(message);
        }

        jam::score score_at { base_score };
        std::uint64_t patches { 0 };

        for (const auto& entry : prefix.entries)
        {
            if (entry.kind != replay::entry_kind::set_input ||
                entry.port != "patch")
                continue;

            jam::score_patch patch {};

            if (user_bytes_as(entry.value, patch))
            {
                score_at = jam::apply(score_at, patch);
                ++patches;
            }
        }

        bool truncated { false };

        std::string out { "{\"ok\":true,\"block\":" };
        append_uint(out, index);
        out += ",\"blocks\":";
        append_uint(out, audio_steps.size());
        out += ",\"cut\":";
        append_uint(out, cut.value);
        out += ",\"entries\":";
        append_uint(out, prefix.entries.size());
        out += ",\"patches\":";
        append_uint(out, patches);
        out += ",\"digest\":\"";
        out += digest_hex(run_digest(run));
        out += "\",\"score\":";
        append_score(out, score_at);
        out += ",\"ticks\":";
        out += audio_ticks_json(prefix, run, max_branch_ticks, truncated);
        out += ",\"truncated\":";
        append_bool(out, truncated);
        out.push_back('}');
        return out;
    }

    std::string
    reset()
    {
        m_intents = intents {};
        restart_with_score(make_demo_score(), true);
        return state_json();
    }

private:
    static double
    clamp_bpm(double bpm)
    {
        return std::clamp(bpm, 20.0, 400.0);
    }

    void
    register_codecs()
    {
        m_codecs.register_user<jam::score_patch>("jam.score_patch");
        m_codecs.register_user<jam::event_batch>("jam.event_batch");
        m_codecs.register_user<jam::voice_state>("jam.voice_state");
        m_codecs.register_user<daw::LoopRegion>("daw.loop_region");
        m_codecs.register_user<daw::BlockDesc>("daw.block_desc");
    }

    void
    start_journal()
    {
        m_builder = std::make_unique<replay::scenario_builder>(
            replay::run_id { 1 }, replay::branch_id { 1 }
        );
        m_session = std::make_unique<replay::session>(
            *m_program, m_engine, m_codecs, *m_builder
        );
        m_live_run = replay::run {};
        m_live_run.run = replay::run_id { 1 };
        m_live_run.branch = replay::branch_id { 1 };
        m_ordinals.fill(0);
        m_block_index = 0;
        m_next_frame = 0;
        m_patch_count = 0;
        m_initialized = false;
        m_imported_scenario.reset();
        m_imported_run.reset();
        m_branch_scenario.reset();
        m_branch_run.reset();
        m_branch_original_scenario.reset();
        m_branch_original_run.reset();
    }

    void
    restart_with_score(const jam::score& score, bool initialize_now)
    {
        m_session.reset();
        m_builder.reset();
        m_program.reset();

        m_initial = score;
        m_score = score;
        m_recipe = jam::net::app(m_initial);
        m_program =
            std::make_unique<kernel::program>(m_recipe, make_program_options());

        start_journal();

        if (initialize_now)
            initialize();
    }

    void
    initialize()
    {
        m_session->begin_epoch(clock_tag::ui);
        m_session->mark_initialized();
        m_session->set("playing", value { m_intents.playing });
        m_session->set("bpm", value { m_intents.bpm });
        m_session->set<daw::LoopRegion>(
            "loop",
            m_intents.loop_start,
            m_intents.loop_end,
            m_intents.loop_enabled
        );
        m_session->set("master", value { m_intents.master });
        m_session->step(clock_tag::ui);
        capture(clock_tag::ui);
        m_initialized = true;
    }

    void
    apply_intents()
    {
        if (!m_initialized)
        {
            initialize();
            return;
        }

        m_session->begin_epoch(clock_tag::ui);
        m_session->set("playing", value { m_intents.playing });
        m_session->set("bpm", value { m_intents.bpm });
        m_session->set<daw::LoopRegion>(
            "loop",
            m_intents.loop_start,
            m_intents.loop_end,
            m_intents.loop_enabled
        );
        m_session->set("master", value { m_intents.master });
        m_session->step(clock_tag::ui);
        capture(clock_tag::ui);
    }

    std::string
    submit_patch(const jam::score_patch& patch)
    {
        if (!m_initialized)
            initialize();

        m_session->begin_epoch(clock_tag::ui);
        m_session->set<jam::score_patch>("patch", patch);
        m_session->step(clock_tag::ui);
        capture(clock_tag::ui);

        m_score = jam::apply(m_score, patch);
        ++m_patch_count;

        std::string out { "{\"ok\":true,\"patch\":" };
        append_patch(out, patch);
        out += ",\"revision\":";
        append_uint(out, m_patch_count);
        out += ",\"score\":";
        append_score(out, m_score);
        out.push_back('}');
        return out;
    }

    void
    capture(clock_tag clock)
    {
        const auto index { kernel::clock_tag_index(clock) };

        replay::step_record record;
        record.step = replay::region_step_id { clock, m_ordinals[index]++ };
        record.epoch = m_program->region(clock).published_epoch.load(
            std::memory_order_acquire
        );

        const auto output_count { m_program->descriptor().output_ids.size() };
        record.outputs.reserve(output_count);

        for (std::size_t i { 0 }; i < output_count; ++i)
        {
            replay::encoded_value encoded;
            std::string error;

            if (!replay::encode_value(
                    m_program->get(i), m_codecs, encoded, error
                ))
                throw std::runtime_error { "cannot encode jam output " +
                                           std::to_string(i) + ": " + error };

            record.outputs.push_back(std::move(encoded));
        }

        m_live_run.steps.push_back(std::move(record));
    }

    [[nodiscard]] const replay::scenario&
    recorded_scenario() const
    {
        return m_session->recorded();
    }

    static std::string
    history_json(const replay::scenario& scenario)
    {
        std::uint64_t blocks { 0 };

        for (const auto& entry : scenario.entries)
            if (entry.kind == replay::entry_kind::step_region &&
                entry.clock == clock_tag::audio)
                ++blocks;

        std::string out { "{\"blocks\":" };
        append_uint(out, blocks);
        out += ",\"patches\":[";

        bool first { true };
        std::uint64_t seen_blocks { 0 };
        std::uint64_t revision { 0 };

        for (const auto& entry : scenario.entries)
        {
            if (entry.kind == replay::entry_kind::step_region &&
                entry.clock == clock_tag::audio)
            {
                ++seen_blocks;
                continue;
            }

            if (entry.kind != replay::entry_kind::set_input ||
                entry.port != "patch")
                continue;

            ++revision;

            if (!first)
                out.push_back(',');

            first = false;
            out += "{\"revision\":";
            append_uint(out, revision);
            out += ",\"block\":";
            append_uint(out, seen_blocks);
            out += ",\"entry\":";
            append_uint(out, entry.id.value);
            out.push_back('}');
        }

        out += "]}";
        return out;
    }

    static std::uint64_t
    default_branch_cut(const replay::scenario& scenario)
    {
        std::uint64_t previous { 0 };

        for (const auto& entry : scenario.entries)
        {
            if (entry.kind == replay::entry_kind::step_region &&
                entry.clock == clock_tag::audio)
                return previous;

            if (entry.kind != replay::entry_kind::mark)
                previous = entry.id.value;
        }

        return 0;
    }

    bool
    encode_patch(
        const jam::score_patch& patch,
        replay::encoded_value& out,
        std::string& error
    )
    {
        const auto value {
            kernel::make_user_value<jam::score_patch>(nullptr, patch)
        };

        const bool ok { replay::encode_value(value, m_codecs, out, error) };
        kernel::release(value);
        return ok;
    }

    replay::run
    replay_with_score(
        const replay::scenario& scenario,
        const jam::score& score
    ) const
    {
        const auto recipe { jam::net::app(score) };
        replay::native::recipe_factory factory { recipe };
        replay::replayer replayer;
        replay::replay_options options;
        options.capture_outputs = true;
        options.capture_trace = false;
        options.program_options = make_program_options();
        return replayer.replay(scenario, factory, m_codecs, options);
    }

    replay::run
    replay_branch_with_score(
        const replay::scenario& base,
        const replay::branch_edit& edit,
        replay::branch_id branch,
        const jam::score& score
    )
    {
        const auto recipe { jam::net::app(score) };
        replay::native::recipe_factory factory { recipe };
        replay::replayer replayer;
        replay::replay_options options;
        options.capture_outputs = true;
        options.capture_trace = false;
        options.program_options = make_program_options();
        return replayer.replay_branch(
            base, edit, branch, factory, m_codecs, options
        );
    }

    static std::string
    score_json(const jam::score& score)
    {
        std::string out;
        append_score(out, score);
        return out;
    }

    std::string
    state_json() const
    {
        const auto outputs { read_tick_outputs(*m_program) };
        const replay::scenario& recorded {
            m_imported_scenario ? *m_imported_scenario : recorded_scenario()
        };

        std::uint64_t patches { 0 };

        for (const auto& entry : recorded.entries)
            if (entry.kind == replay::entry_kind::set_input &&
                entry.port == "patch")
                ++patches;

        std::string out { "{\"ok\":true,\"initialized\":" };
        append_bool(out, m_initialized);
        out += ",\"score\":";
        append_score(out, m_score);
        out += ",\"intents\":{\"playing\":";
        append_bool(out, m_intents.playing);
        out += ",\"bpm\":";
        append_number(out, m_intents.bpm);
        out += ",\"loop\":{\"start\":";
        append_number(out, m_intents.loop_start);
        out += ",\"end\":";
        append_number(out, m_intents.loop_end);
        out += ",\"enabled\":";
        append_bool(out, m_intents.loop_enabled);
        out += "},\"master\":";
        append_number(out, m_intents.master);
        out += "},\"playhead\":";
        append_number(out, outputs.playhead);
        out += ",\"playheadUi\":";
        append_number(out, outputs.playhead_ui);
        out += ",\"mix\":";
        append_number(out, outputs.mix);
        out += ",\"eventCount\":";
        append_uint(
            out,
            outputs.has_events ? std::min<std::size_t>(
                                     outputs.events.count, jam::event_capacity
                                 )
                               : 0
        );
        out += ",\"dropped\":";
        append_uint(out, outputs.has_events ? outputs.events.dropped : 0);
        out += ",\"events\":";
        if (outputs.has_events)
            append_events(out, outputs.events);
        else
            out += "[]";
        out += ",\"voices\":";
        append_voices(out, outputs.voices);
        out += ",\"block\":{\"index\":";
        append_uint(out, m_block_index);
        out += ",\"start\":";
        append_int(out, m_next_frame);
        out += ",\"frames\":";
        append_int(out, frames_per_block);
        out += ",\"rate\":";
        append_number(out, sample_rate);
        out += "},\"journal\":{\"run\":";
        append_uint(out, recorded.run.value);
        out += ",\"branch\":";
        append_uint(out, recorded.branch.value);
        out += ",\"entries\":";
        append_uint(out, recorded.entries.size());
        out += ",\"patches\":";
        append_uint(out, patches);
        out += ",\"complete\":";
        append_bool(out, recorded.fidelity.complete);
        out += "},\"history\":";
        out += history_json(recorded);
        out += ",\"imported\":";
        append_bool(out, m_imported_scenario.has_value());
        out += ",\"branchRun\":";
        append_bool(out, m_branch_run.has_value());
        out.push_back('}');
        return out;
    }

    jam::score m_initial;
    jam::score m_score;
    intents m_intents;
    recipe_type m_recipe;
    std::unique_ptr<kernel::program> m_program;
    kernel::release_engine m_engine;
    replay::codec_registry m_codecs;
    std::unique_ptr<replay::scenario_builder> m_builder;
    std::unique_ptr<replay::session> m_session;
    replay::run m_live_run;
    std::array<std::uint64_t, kernel::clock_tag_count> m_ordinals {};
    std::size_t m_block_index { 0 };
    std::int64_t m_next_frame { 0 };
    std::size_t m_patch_count { 0 };
    bool m_initialized { false };
    std::optional<replay::scenario> m_imported_scenario;
    std::optional<replay::run> m_imported_run;
    jam::score m_imported_score;
    std::optional<replay::scenario> m_branch_scenario;
    std::optional<replay::run> m_branch_run;
    std::optional<replay::scenario> m_branch_original_scenario;
    std::optional<replay::run> m_branch_original_run;
};

} // namespace

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
EMSCRIPTEN_BINDINGS(jam_shell)
{
    emscripten::class_<jam_shell>("JamShell")
        .constructor()
        .function("createScore", &jam_shell::create_score)
        .function("defaultScoreJson", &jam_shell::default_score_json)
        .function("init", &jam_shell::init)
        .function("setIntents", &jam_shell::set_intents)
        .function("setScoreJson", &jam_shell::set_score_json)
        .function("setPatchJson", &jam_shell::set_patch_json)
        .function("setStep", &jam_shell::set_step)
        .function("step", &jam_shell::step)
        .function("getStateJson", &jam_shell::get_state_json)
        .function("exportScenarioJson", &jam_shell::export_scenario_json)
        .function("importScenarioJson", &jam_shell::import_scenario_json)
        .function("replayBranchJson", &jam_shell::replay_branch_json)
        .function("replayBlockJson", &jam_shell::replay_block_json)
        .function(
            "compareWithOriginalJson", &jam_shell::compare_with_original_json
        )
        .function("reset", &jam_shell::reset);
}
#pragma clang diagnostic pop
