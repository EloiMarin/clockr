#include "clockr/replay/value_codec.hpp"

#include "test_support.hpp"

#include "clockr/kernel/program.hpp"
#include "clockr/kernel/value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace clockr;
using namespace clockr::replay;

namespace replay_test
{

struct padded
{
    int value { 0 };
    bool flag { false };
};

} // namespace replay_test

namespace clockr::kernel
{

template<>
struct describe<replay_test::padded>
{
    static const user_type&
    type()
    {
        static const std::array fields
        {
            field("value", &replay_test::padded::value),
            field("flag", &replay_test::padded::flag),
        };
        static const user_type t {
            make_user_type<replay_test::padded>("test.padded", fields)
        };

        return t;
    }
};

} // namespace clockr::kernel

namespace
{

bool
same_payload(const encoded_value& lhs, const encoded_value& rhs)
{
    return encoded_value_equal(lhs, rhs);
}

} // namespace

TEST_CASE(
    "replay | Scalar values round-trip through the journal",
    "[replay][codec]"
)
{
    codec_registry codecs;
    encoded_value encoded;
    std::string error;

    REQUIRE(encode_value(
        kernel::value { std::monostate {} }, codecs, encoded, error
    ));
    REQUIRE(std::holds_alternative<std::monostate>(encoded.payload));

    REQUIRE(encode_value(kernel::value { true }, codecs, encoded, error));
    REQUIRE(std::get<bool>(encoded.payload));

    REQUIRE(encode_value(
        kernel::value { std::int64_t { -7 } }, codecs, encoded, error
    ));
    REQUIRE(std::get<std::int64_t>(encoded.payload) == -7);

    REQUIRE(encode_value(kernel::value { 2.5 }, codecs, encoded, error));
    REQUIRE(std::get<double>(encoded.payload) == 2.5);

    REQUIRE(equal_value(kernel::value { 2.5 }, encoded, codecs, error));
}

TEST_CASE(
    "replay | A POD struct round-trips into a fresh program",
    "[replay][codec]"
)
{
    codec_registry codecs;
    codecs.register_user<replay_test::point>("test.point");

    const kernel::value live {
        kernel::make_user_value<replay_test::point>(nullptr, 3, 4)
    };

    encoded_value encoded;
    std::string error;

    REQUIRE(encode_value(live, codecs, encoded, error));
    REQUIRE(std::holds_alternative<user_bytes>(encoded.payload));
    REQUIRE(equal_value(live, encoded, codecs, error));

    kernel::program program { replay_test::network() };

    kernel::value decoded;
    REQUIRE(decode_value(
        encoded, program, kernel::clock_tag::ui, codecs, decoded, error
    ));
    REQUIRE(equal_value(decoded, encoded, codecs, error));
    REQUIRE(std::holds_alternative<kernel::user_inline>(decoded));

    kernel::release(decoded);
    kernel::release(live);
}

TEST_CASE(
    "replay | A missing codec reports failure instead of guessing",
    "[replay][codec]"
)
{
    codec_registry codecs;

    const kernel::value live {
        kernel::make_user_value<replay_test::point>(nullptr, 1, 2)
    };

    encoded_value encoded;
    std::string error;

    REQUIRE_FALSE(encode_value(live, codecs, encoded, error));
    REQUIRE(error.find("no replay codec") != std::string::npos);

    user_bytes bytes;
    bytes.type_id = 12345;
    bytes.bytes.push_back(std::byte { 1 });
    encoded_value journal;
    journal.payload = std::move(bytes);

    kernel::program program { replay_test::network() };
    kernel::value decoded;

    REQUIRE_FALSE(decode_value(
        journal, program, kernel::clock_tag::ui, codecs, decoded, error
    ));
    REQUIRE(error.find("no replay codec") != std::string::npos);

    kernel::release(live);
}

TEST_CASE(
    "replay | Described struct padding never reaches the journal",
    "[replay][codec]"
)
{
    codec_registry codecs;
    codecs.register_user<replay_test::padded>("test.padded");

    replay_test::padded first {};
    replay_test::padded second {};

    std::memset(&first, 0xAA, sizeof(first));
    std::memset(&second, 0x55, sizeof(second));

    first.value = 7;
    first.flag = true;
    second.value = 7;
    second.flag = true;

    const kernel::value first_value {
        kernel::make_user_value<replay_test::padded>(nullptr, first)
    };
    const kernel::value second_value {
        kernel::make_user_value<replay_test::padded>(nullptr, second)
    };

    encoded_value first_encoded;
    encoded_value second_encoded;
    std::string error;

    REQUIRE(encode_value(first_value, codecs, first_encoded, error));
    REQUIRE(encode_value(second_value, codecs, second_encoded, error));
    REQUIRE(same_payload(first_encoded, second_encoded));

    kernel::release(first_value);
    kernel::release(second_value);
}

TEST_CASE(
    "replay | Equal structs in different cells compare equal",
    "[replay][codec]"
)
{
    codec_registry codecs;
    codecs.register_user<replay_test::big_point>("test.big_point");

    replay_test::big_point object {};
    object.values[0] = 1;
    object.values[7] = 9;

    const kernel::value first {
        kernel::make_user_value<replay_test::big_point>(nullptr, object)
    };
    const kernel::value second {
        kernel::make_user_value<replay_test::big_point>(nullptr, object)
    };

    REQUIRE(std::holds_alternative<kernel::user_ref>(first));
    REQUIRE(std::holds_alternative<kernel::user_ref>(second));
    REQUIRE(
        std::get<kernel::user_ref>(first).cell !=
        std::get<kernel::user_ref>(second).cell
    );

    encoded_value encoded;
    std::string error;

    REQUIRE(encode_value(first, codecs, encoded, error));
    REQUIRE(equal_value(second, encoded, codecs, error));

    encoded_value other_encoded;
    REQUIRE(encode_value(second, codecs, other_encoded, error));
    REQUIRE(same_payload(encoded, other_encoded));

    kernel::release(first);
    kernel::release(second);
}
