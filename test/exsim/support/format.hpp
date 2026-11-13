#ifndef EXSIM_TEST_SUPPORT_FORMAT_HPP
#define EXSIM_TEST_SUPPORT_FORMAT_HPP

// Human-readable one-line formatting of every Command, Event, OrderInfo,
// LevelInfo and AuctionIndicative (all fields), plus field-by-field
// equality and difference descriptions.
//
//   NEW_ORDER ts=3 instrument=1 session=2 owner=0 cl_ord_id=7 side=BUY ...
//   FILL instrument=1 ts=9 match_id=4 price=100.25 qty=40 aggressor=SELL buy.order_id=1 ...
//
// Test diagnostics only. Built on server::journal::visit_fields, so
// formatting, equality and the canonical event hash all agree on what
// "every field" means.

#include <engine/command.hpp>
#include <engine/event.hpp>
#include <engine/matching_engine.hpp>
#include <server/journal/fields.hpp>
#include <engine/price_text.hpp>

#include <cstdint>
#include <string>

namespace exsim::testsupport {

[[nodiscard]] inline std::string format_field_value(const server::journal::Field& field) {
    if (field.kind == server::journal::FieldKind::Price) {
        return format_price(static_cast<Price>(field.raw), 2);
    }
    if (field.kind == server::journal::FieldKind::Bool) {
        return field.raw == 0 ? "false" : (field.raw == 1 ? "true" : "?" + std::to_string(field.raw));
    }
    if (server::journal::is_enum_kind(field.kind)) {
        const std::string_view name = server::journal::enum_value_name(field.kind, field.raw);
        return name.empty() ? "?" + std::to_string(field.raw) : std::string(name);
    }
    return std::to_string(field.raw);
}

template <typename T>
[[nodiscard]] std::string format_fields(const T& value) {
    std::string text;
    server::journal::visit_fields(value, [&](const server::journal::Field& field) {
        if (!text.empty()) text += ' ';
        text += field.name;
        text += '=';
        text += format_field_value(field);
    });
    return text;
}

[[nodiscard]] inline std::string format_event_type(EventType type) {
    const std::string_view name = server::journal::event_type_name(type);
    if (name == "?") return "EVENT?" + std::to_string(static_cast<unsigned>(type));
    std::string upper(name);
    for (char& ch : upper) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    return upper;
}

[[nodiscard]] inline std::string format_event(const Event& event) {
    return format_event_type(event.type) + " " + format_fields(event);
}

[[nodiscard]] inline std::string format_command(const Command& command) {
    const std::string_view name = to_string(command.type);
    std::string head = name == "?" ? "COMMAND?" + std::to_string(static_cast<unsigned>(command.type)) : std::string(name);
    const std::string body = format_fields(command);
    return body.empty() ? head : head + " " + body;
}

}  // namespace exsim::testsupport

#endif
