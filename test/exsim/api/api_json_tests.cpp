// JSON writer, exact decimal formatting and parsing, SHA-256, cursors and the
// experiment configuration translation.

#include "exsim/api/api_json.hpp"
#include "exsim/api/experiment_config_translation.hpp"
#include "exsim/api/research_run.hpp"
#include "exsim/api/sha256.hpp"
#include "exsim/research/json.hpp"
#include "test/framework/test.hpp"

#include <string>

using namespace exsim::api;
using exsim::research::JsonValue;
using exsim::research::parse_json;

TEST_CASE(api_json, sha256_known_vectors) {
    CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256_hex(std::string(1'000'000, 'a')), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

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

TEST_CASE(api_json, cursors_are_bound_to_their_filters) {
    const std::string cursor = encode_page_cursor(42, "events|kinds=order_filled");
    CHECK_EQ(decode_page_cursor(cursor, "events|kinds=order_filled").value_or(0), 42u);
    CHECK(!decode_page_cursor(cursor, "events|kinds=order_canceled").has_value());
    CHECK(!decode_page_cursor("c42", "events|kinds=order_filled").has_value());
    CHECK(!decode_page_cursor("garbage", "x").has_value());
    CHECK(!decode_page_cursor("", "x").has_value());
    CHECK(!decode_page_cursor("c99999999999999999999999_00000000", "x").has_value());
}

TEST_CASE(api_translation, baselines_translate_and_execute) {
    REQUIRE(baseline_configurations().size() >= 2u);
    for (const BaselineConfiguration& baseline : baseline_configurations()) {
        const ConfigTranslationResult translation = translate_experiment_config(parse_json(baseline.config_json), "/config");
        CHECK(translation.shape_issues.empty());
        CHECK(translation.semantic_issues.empty());
        CHECK(translation.market_world_supported);
        REQUIRE(translation.translated.has_value());
        CHECK_EQ(translation.translated->config_digest_hex.size(), 64u);
        CHECK_EQ(translation.translated->market_world, std::string("interactive_synthetic"));
        CHECK(!translation.translated->master_seeds.empty());
        CHECK(find_baseline(baseline.baseline_id) == &baseline);
    }
    CHECK(find_baseline("nope") == nullptr);
}

namespace {

std::string replace_once(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

bool has_issue_at(const std::vector<FieldIssue>& issues, std::string_view field_path) {
    for (const FieldIssue& issue : issues)
        if (issue.field_path == field_path) return true;
    return false;
}

}  // namespace

TEST_CASE(api_translation, shape_and_semantic_issues_carry_json_pointers) {
    const std::string config_json = baseline_configurations().front().config_json;
    const auto unknown_field = translate_experiment_config(parse_json(replace_once(config_json, "\"postOnly\":", "\"bogus\":1,\"postOnly\":")), "/config");
    CHECK(has_issue_at(unknown_field.shape_issues, "/config/strategy/bogus"));
    CHECK(!unknown_field.translated.has_value());

    const auto wrong_type = translate_experiment_config(parse_json(replace_once(config_json, "\"postOnly\":true", "\"postOnly\":\"yes\"")), "/config");
    CHECK(has_issue_at(wrong_type.shape_issues, "/config/strategy/postOnly"));

    const auto historical = translate_experiment_config(
        parse_json(replace_once(config_json, "\"marketWorld\":\"interactive_synthetic\"", "\"marketWorld\":\"historical_reconstruction\"")), "/config");
    CHECK(historical.shape_issues.empty());
    CHECK(!historical.market_world_supported);
    CHECK(has_issue_at(historical.semantic_issues, "/config/marketWorld"));

    const auto bad_mark = translate_experiment_config(
        parse_json(replace_once(config_json, "\"markConvention\":\"exchange_mid\"", "\"markConvention\":\"strategy_visible_mid\"")), "/config");
    CHECK(has_issue_at(bad_mark.semantic_issues, "/config/markConvention"));
}

TEST_CASE(api_translation, digest_is_independent_of_member_order) {
    const JsonValue forward = parse_json(R"({"b":"2","a":{"y":[1,2],"x":null}})");
    const JsonValue reordered = parse_json(R"({"a":{"x":null,"y":[1,2]},"b":"2"})");
    const JsonValue changed = parse_json(R"({"a":{"x":null,"y":[2,1]},"b":"2"})");
    CHECK_EQ(configuration_digest_hex(forward), configuration_digest_hex(reordered));
    CHECK_NE(configuration_digest_hex(forward), configuration_digest_hex(changed));
    CHECK_EQ(stable_serialize_json_value(reordered), std::string(R"({"a":{"x":null,"y":[1,2]},"b":"2"})"));
}

TEST_CASE(api_translation, assumption_differences_between_the_two_baselines) {
    const auto& baselines = baseline_configurations();
    const auto differences = list_assumption_differences(parse_json(baselines[0].config_json), parse_json(baselines[1].config_json));
    bool spread_differs = false;
    for (const AssumptionDifference& difference : differences) {
        CHECK_NE(difference.baseline_value, difference.candidate_value);
        if (difference.reference.field_path == "/strategy/halfSpreadTicks") spread_differs = true;
    }
    CHECK(spread_differs);
    CHECK(list_assumption_differences(parse_json(baselines[0].config_json), parse_json(baselines[0].config_json)).empty());
}
