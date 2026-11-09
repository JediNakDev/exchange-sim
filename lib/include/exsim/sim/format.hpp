#ifndef EXSIM_SIM_FORMAT_HPP
#define EXSIM_SIM_FORMAT_HPP

// Human-readable one-line formatting of every Command, Event, OrderInfo,
// LevelInfo and AuctionIndicative (all fields), plus field-by-field
// equality and difference descriptions.
//
//   NEW_ORDER ts=3 instrument=1 session=2 owner=0 cl_ord_id=7 side=BUY ...
//   FILL instrument=1 ts=9 match_id=4 price=100.25 qty=40 aggressor=SELL buy.order_id=1 ...
//
// Shared by scenario parsing, diagnostic tools and tests.
// Built on exsim::sim::visit_fields, so formatting, equality and the
// canonical event hash all agree on what "every field" means.

#include <exsim/command.hpp>
#include <exsim/event.hpp>
#include <exsim/matching_engine.hpp>
#include <exsim/sim/fields.hpp>
#include <exsim/sim/price_text.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace exsim::sim {

using sim::commands_equal;
using sim::events_equal;
using sim::order_infos_equal;

[[nodiscard]] inline std::string format_field_value(const sim::Field& field) {
    if (field.kind == sim::FieldKind::Price) {
        return sim::format_price(static_cast<Price>(field.raw));
    }
    if (field.kind == sim::FieldKind::Bool) {
        return field.raw == 0 ? "false" : (field.raw == 1 ? "true" : "?" + std::to_string(field.raw));
    }
    if (sim::is_enum_kind(field.kind)) {
        const std::string_view name = sim::enum_value_name(field.kind, field.raw);
        return name.empty() ? "?" + std::to_string(field.raw) : std::string(name);
    }
    return std::to_string(field.raw);
}

template <typename T>
[[nodiscard]] std::string format_fields(const T& value) {
    std::string text;
    sim::visit_fields(value, [&](const sim::Field& field) {
        if (!text.empty()) text += ' ';
        text += field.name;
        text += '=';
        text += format_field_value(field);
    });
    return text;
}

[[nodiscard]] inline std::string format_event_type(EventType type) {
    const std::string_view name = sim::event_type_name(type);
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

[[nodiscard]] inline std::string format_order(const OrderInfo& order) { return "ORDER " + format_fields(order); }

[[nodiscard]] inline std::string format_level(const LevelInfo& level) {
    return sim::format_price(level.price) + " qty=" + std::to_string(level.qty) +
           " orders=" + std::to_string(level.order_count);
}

[[nodiscard]] inline std::string format_indicative(const AuctionIndicative& a) {
    return std::string("crossed=") + (a.crossed ? "true" : "false") + " price=" + sim::format_price(a.price) +
           " paired=" + std::to_string(a.paired_qty) + " imbalance=" + std::to_string(a.imbalance_qty) +
           " side=" + std::string(to_string(a.imbalance_side));
}

[[nodiscard]] inline std::string format_optional_price(const std::optional<Price>& price) {
    return price ? sim::format_price(*price) : std::string("none");
}

// Every event on its own line, prefixed by `indent` and its index.
[[nodiscard]] inline std::string format_events(const Event* begin, const Event* end, const std::string& indent = "  ") {
    std::string text;
    std::size_t index = 0;
    for (const Event* e = begin; e != end; ++e, ++index) {
        text += indent + "[" + std::to_string(index) + "] " + format_event(*e) + "\n";
    }
    if (begin == end) text += indent + "(no events)\n";
    return text;
}

// "price: 100.25 != 100.3; qty: 40 != 30" (empty when equal).
template <typename T>
[[nodiscard]] std::string describe_field_differences(const T& a, const T& b) {
    struct Item {
        std::string_view name;
        sim::Field field;
    };
    Item items_a[32];
    std::size_t count_a = 0;
    sim::visit_fields(a, [&](const sim::Field& field) {
        if (count_a < 32) items_a[count_a++] = Item{field.name, field};
    });
    std::string text;
    std::size_t index = 0;
    sim::visit_fields(b, [&](const sim::Field& field) {
        if (index < count_a && items_a[index].field.raw != field.raw) {
            if (!text.empty()) text += "; ";
            text += std::string(field.name) + ": " + format_field_value(items_a[index].field) +
                    " != " + format_field_value(field);
        }
        ++index;
    });
    return text;
}

[[nodiscard]] inline std::string describe_event_difference(const Event& a, const Event& b) {
    if (a.type != b.type) return "type: " + format_event_type(a.type) + " != " + format_event_type(b.type);
    return describe_field_differences(a, b);
}

}  // namespace exsim::sim

#endif
