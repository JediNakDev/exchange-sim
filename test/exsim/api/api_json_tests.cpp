// JSON writer, exact decimal formatting and parsing, SHA-256, cursors and the
// experiment configuration translation.

#include "api/api_json.hpp"
#include "api/json.hpp"
#include "test/framework/test.hpp"

#include <string>

using namespace exsim::api;
using exsim::api::JsonValue;
using exsim::api::parse_json;

TEST_CASE(api_json, writer_escapes_and_nests) {
    ApiJsonWriter json;
    json.begin_object();
    json.key("text").string(std::string("quote\" backslash\\ newline\n tab\t nul") + std::string(1, '\0') + "\x1f");
    json.key("list").begin_array().integer_number(-3).unsigned_text(18446744073709551615ull).signed_text(-9).null().boolean(true).end_array();
    json.key("missing").optional_string(std::nullopt);
    json.key("present").optional_string(std::string("x"));
    json.end_object();
    const std::string text = json.take();
    CHECK_EQ(text, std::string(R"({"text":"quote\" backslash\\ newline\n tab\t nul\u0000\u001f","list":[-3,"18446744073709551615","-9",null,true],"missing":null,"present":"x"})"));
    const JsonValue parsed = parse_json(text);
    CHECK_EQ(parsed.find("list")->items.size(), 5u);
}

TEST_CASE(api_json, fixed_point_formatting) {
    CHECK_EQ(format_price_units(1'000'000), std::string("100.0000"));
    CHECK_EQ(format_price_units(-25), std::string("-0.0025"));
    CHECK_EQ(format_price_units(0), std::string("0.0000"));
    CHECK_EQ(format_doubled_price_units(2'000'001), std::string("100.00005"));
    CHECK_EQ(format_doubled_price_units(-1), std::string("-0.00005"));
    CHECK_EQ(format_price_units_at_mid_scale(1'000'000), std::string("100.00000"));
    CHECK_EQ(format_scaled_integer(-123456, 3), std::string("-123.456"));
}

TEST_CASE(api_json, derived_decimals_round_half_to_even) {
    CHECK_EQ(format_derived_decimal(0.0), std::string("0.000000"));
    CHECK_EQ(format_derived_decimal(1.0 / 3.0), std::string("0.333333"));
    CHECK_EQ(format_derived_decimal(-2.5), std::string("-2.500000"));
    CHECK_EQ(format_derived_decimal(0.0000005), std::string("0.000000"));  // half to even
    CHECK_EQ(format_derived_decimal(0.0000015), std::string("0.000002"));
    CHECK_EQ(format_derived_decimal(-0.0000001), std::string("0.000000"));  // no negative zero
}

TEST_CASE(api_json, integer_text_parsing_is_strict) {
    std::uint64_t unsigned_value = 0;
    CHECK(parse_unsigned_integer_text("0", unsigned_value) && unsigned_value == 0);
    CHECK(parse_unsigned_integer_text("18446744073709551615", unsigned_value) && unsigned_value == 18446744073709551615ull);
    CHECK(!parse_unsigned_integer_text("18446744073709551616", unsigned_value));
    CHECK(!parse_unsigned_integer_text("01", unsigned_value));
    CHECK(!parse_unsigned_integer_text("", unsigned_value));
    CHECK(!parse_unsigned_integer_text("-1", unsigned_value));
    CHECK(!parse_unsigned_integer_text("1e3", unsigned_value));
    CHECK(!parse_unsigned_integer_text(" 1", unsigned_value));
    std::int64_t signed_value = 0;
    CHECK(parse_signed_integer_text("-9223372036854775808", signed_value) && signed_value == INT64_MIN);
    CHECK(!parse_signed_integer_text("9223372036854775808", signed_value));
    CHECK(!parse_signed_integer_text("-0", signed_value));
}

TEST_CASE(api_json, scaled_decimals_subtract_and_compare_exactly) {
    const auto left = parse_scaled_decimal("1694.06200");
    const auto right = parse_scaled_decimal("1423.327");
    REQUIRE(left.has_value() && right.has_value());
    const auto difference = subtract_scaled_decimals(*right, *left);
    REQUIRE(difference.has_value());
    CHECK_EQ(format_scaled_decimal(*difference), std::string("-270.73500"));
    CHECK_EQ(compare_scaled_decimals(*left, *right), 1);
    CHECK_EQ(compare_scaled_decimals(*parse_scaled_decimal("1.50"), *parse_scaled_decimal("1.5")), 0);
    CHECK(!parse_scaled_decimal("1.").has_value());
    CHECK(!parse_scaled_decimal("01.5").has_value());
    CHECK(!parse_scaled_decimal("1234567890123456789").has_value());
    // Rescaling 900000000000000000 to five fraction digits does not fit in 64 bits.
    const auto huge = parse_scaled_decimal("900000000000000000");
    const auto tiny = parse_scaled_decimal("-0.00001");
    REQUIRE(huge.has_value() && tiny.has_value());
    CHECK(!subtract_scaled_decimals(*huge, *tiny).has_value());
}

TEST_CASE(api_json, wall_clock_time_is_rfc3339_milliseconds) {
    CHECK_EQ(format_wall_clock_time(0), std::string("1970-01-01T00:00:00.000Z"));
    CHECK_EQ(format_wall_clock_time(1'793'967'034'885), std::string("2026-11-06T12:10:34.885Z"));
}
