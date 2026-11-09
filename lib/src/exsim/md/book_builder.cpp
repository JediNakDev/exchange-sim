#include <exsim/md/book_builder.hpp>

#include <limits>
#include <stdexcept>

namespace exsim::md {

namespace {
constexpr std::size_t side_index(Side side) noexcept { return side == Side::BUY ? 0 : 1; }
// Bids are kept highest first, asks lowest first.
constexpr bool better_price(Side side, Price candidate, Price reference) noexcept {
    return side == Side::BUY ? candidate > reference : candidate < reference;
}
}  // namespace

ItchBookBuilder::ItchBookBuilder(const BookBuilderConfig& config)
    : max_instruments_(config.max_instruments),
      max_orders_(config.max_orders),
      max_levels_per_side_(config.max_levels_per_side) {
    if (max_instruments_ == 0 || max_instruments_ > std::numeric_limits<InstrumentId>::max())
        throw std::invalid_argument("book builder max_instruments out of range");
    if (max_orders_ == 0 || max_orders_ >= kNoOrderSlot) throw std::invalid_argument("book builder max_orders out of range");
    if (max_levels_per_side_ == 0 ||
        max_levels_per_side_ > std::numeric_limits<std::size_t>::max() / sizeof(Level) / 2 / max_instruments_)
        throw std::invalid_argument("book builder max_levels_per_side out of range");
    orders_ = std::make_unique<OrderSlot[]>(max_orders_);
    levels_ = std::make_unique<Level[]>(max_instruments_ * 2 * max_levels_per_side_);
    instruments_ = std::make_unique<InstrumentRecord[]>(max_instruments_ + 1);
    order_slots_by_reference_.reserve(max_orders_);
    for (std::size_t slot = 0; slot < max_orders_; ++slot)
        orders_[slot].next = slot + 1 < max_orders_ ? static_cast<std::uint32_t>(slot + 1) : kNoOrderSlot;
    free_order_slot_ = 0;
}

ItchBookBuilder::~ItchBookBuilder() = default;

void ItchBookBuilder::on_message(std::uint64_t, std::span<const char> message) noexcept { (void)apply(message); }

BookUpdateStatus ItchBookBuilder::record(BookUpdateStatus status) noexcept {
    if (status == BookUpdateStatus::Applied) {
        ++applied_messages_;
    } else {
        ++rejected_messages_;
        last_rejection_ = status;
    }
    return status;
}

template <class Message>
BookUpdateStatus ItchBookBuilder::decode_then_apply(std::span<const char> bytes) noexcept {
    Message message;
    if (protocol::itch50::decode(bytes, message) != protocol::Status::ok) return record(BookUpdateStatus::Malformed);
    return apply(message);
}

BookUpdateStatus ItchBookBuilder::apply(std::span<const char> message) noexcept {
    namespace itch = protocol::itch50;
    if (message.empty()) return record(BookUpdateStatus::Malformed);
    switch (message[0]) {
        case itch::SystemEvent::type: return decode_then_apply<itch::SystemEvent>(message);
        case itch::StockDirectory::type: return decode_then_apply<itch::StockDirectory>(message);
        case itch::StockTradingAction::type: return decode_then_apply<itch::StockTradingAction>(message);
        case itch::AddOrder::type: return decode_then_apply<itch::AddOrder>(message);
        case itch::AddOrderMPID::type: return decode_then_apply<itch::AddOrderMPID>(message);
        case itch::OrderExecuted::type: return decode_then_apply<itch::OrderExecuted>(message);
        case itch::OrderExecutedWithPrice::type: return decode_then_apply<itch::OrderExecutedWithPrice>(message);
        case itch::OrderCancel::type: return decode_then_apply<itch::OrderCancel>(message);
        case itch::OrderDelete::type: return decode_then_apply<itch::OrderDelete>(message);
        case itch::OrderReplace::type: return decode_then_apply<itch::OrderReplace>(message);
        case itch::Trade::type: return decode_then_apply<itch::Trade>(message);
        case itch::CrossTrade::type: return decode_then_apply<itch::CrossTrade>(message);
        case itch::NetOrderImbalance::type: return decode_then_apply<itch::NetOrderImbalance>(message);
        default: return record(BookUpdateStatus::UnsupportedType);
    }
}

// ---- metadata messages --------------------------------------------------------

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::SystemEvent& message) noexcept {
    last_system_event_ = message.event;
    return record(BookUpdateStatus::Applied);
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::StockDirectory& message) noexcept {
    if (!valid_locate(message.stock_locate)) return record(BookUpdateStatus::UnknownInstrument);
    InstrumentRecord& instrument = instruments_[message.stock_locate];
    instrument.symbol = message.stock;
    instrument.directory_seen = true;
    return record(BookUpdateStatus::Applied);
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::StockTradingAction& message) noexcept {
    if (!valid_locate(message.stock_locate)) return record(BookUpdateStatus::UnknownInstrument);
    instruments_[message.stock_locate].trading_state = message.trading_state;
    return record(BookUpdateStatus::Applied);
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::Trade& message) noexcept {
    // Non-displayed execution: no book effect.
    return record(valid_locate(message.stock_locate) ? BookUpdateStatus::Applied : BookUpdateStatus::UnknownInstrument);
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::CrossTrade& message) noexcept {
    // The constituent executions arrive as C messages; Q only reports the print.
    if (!valid_locate(message.stock_locate)) return record(BookUpdateStatus::UnknownInstrument);
    InstrumentRecord& instrument = instruments_[message.stock_locate];
    instrument.cross = message;
    instrument.cross_seen = true;
    return record(BookUpdateStatus::Applied);
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::NetOrderImbalance& message) noexcept {
    if (!valid_locate(message.stock_locate)) return record(BookUpdateStatus::UnknownInstrument);
    InstrumentRecord& instrument = instruments_[message.stock_locate];
    instrument.imbalance = message;
    instrument.imbalance_seen = true;
    return record(BookUpdateStatus::Applied);
}

// ---- order messages -----------------------------------------------------------

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::AddOrder& message) noexcept {
    return record(add_order(message.stock_locate, message.order_reference, message.side, message.price, message.shares));
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::AddOrderMPID& message) noexcept {
    return record(add_order(message.stock_locate, message.order_reference, message.side, message.price, message.shares));
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::OrderExecuted& message) noexcept {
    return record(reduce_order(message.stock_locate, message.order_reference, message.shares));
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::OrderExecutedWithPrice& message) noexcept {
    return record(reduce_order(message.stock_locate, message.order_reference, message.shares));
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::OrderCancel& message) noexcept {
    return record(reduce_order(message.stock_locate, message.order_reference, message.shares));
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::OrderDelete& message) noexcept {
    return record(delete_order(message.stock_locate, message.order_reference));
}

BookUpdateStatus ItchBookBuilder::apply(const protocol::itch50::OrderReplace& message) noexcept {
    const InstrumentId instrument = message.stock_locate;
    if (!valid_locate(instrument)) return record(BookUpdateStatus::UnknownInstrument);
    const std::uint32_t* original_slot = order_slots_by_reference_.find(message.original_reference);
    if (original_slot == nullptr) return record(BookUpdateStatus::UnknownOrder);
    const OrderSlot original = orders_[*original_slot];
    if (original.instrument != instrument) return record(BookUpdateStatus::InstrumentMismatch);
    if (message.new_reference == 0 || message.shares == 0) return record(BookUpdateStatus::Malformed);
    if (order_slots_by_reference_.contains(message.new_reference)) return record(BookUpdateStatus::DuplicateOrder);

    // Capacity check before any change: does the new price still need a new
    // level once the original order has left its own?
    const Level* original_level = find_level(instrument, original.side, original.price);
    const bool original_level_vanishes = original_level != nullptr && original_level->order_count == 1;
    const Level* target_level = find_level(instrument, original.side, message.price);
    const bool target_level_survives = target_level != nullptr && !(target_level == original_level && original_level_vanishes);
    if (!target_level_survives) {
        const std::size_t levels_after_removal =
            instruments_[instrument].level_counts[side_index(original.side)] - (original_level_vanishes ? 1 : 0);
        if (levels_after_removal >= max_levels_per_side_) return record(BookUpdateStatus::CapacityExceeded);
    }

    // U: the new reference loses priority (joins the tail of its level).
    const std::uint32_t slot = *original_slot;
    remove_order(slot);
    const BookUpdateStatus added =
        add_order(instrument, message.new_reference, original.side, message.price, message.shares);
    return record(added);
}

BookUpdateStatus ItchBookBuilder::add_order(InstrumentId instrument, OrderId reference, Side side, Price price,
                                            Quantity shares) noexcept {
    if (!valid_locate(instrument)) return BookUpdateStatus::UnknownInstrument;
    if (reference == 0 || shares == 0) return BookUpdateStatus::Malformed;
    if (order_slots_by_reference_.contains(reference)) return BookUpdateStatus::DuplicateOrder;
    if (free_order_slot_ == kNoOrderSlot) return BookUpdateStatus::CapacityExceeded;
    if (find_level(instrument, side, price) == nullptr &&
        instruments_[instrument].level_counts[side_index(side)] >= max_levels_per_side_)
        return BookUpdateStatus::CapacityExceeded;
    const std::uint32_t slot = free_order_slot_;
    if (!order_slots_by_reference_.insert(reference, slot)) return BookUpdateStatus::CapacityExceeded;
    free_order_slot_ = orders_[slot].next;
    OrderSlot& order = orders_[slot];
    order.reference = reference;
    order.price = price;
    order.shares = shares;
    order.instrument = instrument;
    order.side = side;
    insert_order(slot);
    ++live_orders_;
    return BookUpdateStatus::Applied;
}

BookUpdateStatus ItchBookBuilder::reduce_order(InstrumentId instrument, OrderId reference, Quantity shares) noexcept {
    if (!valid_locate(instrument)) return BookUpdateStatus::UnknownInstrument;
    const std::uint32_t* slot = order_slots_by_reference_.find(reference);
    if (slot == nullptr) return BookUpdateStatus::UnknownOrder;
    OrderSlot& order = orders_[*slot];
    if (order.instrument != instrument) return BookUpdateStatus::InstrumentMismatch;
    if (shares == 0) return BookUpdateStatus::Malformed;
    if (shares > order.shares) return BookUpdateStatus::SharesExceedOrder;
    if (shares == order.shares) {
        remove_order(*slot);
        return BookUpdateStatus::Applied;
    }
    Level* levels = side_levels(instrument, order.side);
    const std::size_t count = instruments_[instrument].level_counts[side_index(order.side)];
    Level& level = levels[level_position(levels, count, order.side, order.price)];
    level.shares -= shares;
    order.shares -= shares;
    return BookUpdateStatus::Applied;
}

BookUpdateStatus ItchBookBuilder::delete_order(InstrumentId instrument, OrderId reference) noexcept {
    if (!valid_locate(instrument)) return BookUpdateStatus::UnknownInstrument;
    const std::uint32_t* slot = order_slots_by_reference_.find(reference);
    if (slot == nullptr) return BookUpdateStatus::UnknownOrder;
    if (orders_[*slot].instrument != instrument) return BookUpdateStatus::InstrumentMismatch;
    remove_order(*slot);
    return BookUpdateStatus::Applied;
}

// ---- level storage --------------------------------------------------------------

ItchBookBuilder::Level* ItchBookBuilder::side_levels(InstrumentId instrument, Side side) noexcept {
    return levels_.get() + ((static_cast<std::size_t>(instrument) - 1) * 2 + side_index(side)) * max_levels_per_side_;
}

const ItchBookBuilder::Level* ItchBookBuilder::side_levels(InstrumentId instrument, Side side) const noexcept {
    return levels_.get() + ((static_cast<std::size_t>(instrument) - 1) * 2 + side_index(side)) * max_levels_per_side_;
}

std::size_t ItchBookBuilder::level_position(const Level* levels, std::size_t count, Side side,
                                            Price price) const noexcept {
    std::size_t low = 0;
    std::size_t high = count;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (better_price(side, levels[middle].price, price)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

const ItchBookBuilder::Level* ItchBookBuilder::find_level(InstrumentId instrument, Side side,
                                                          Price price) const noexcept {
    if (!valid_locate(instrument)) return nullptr;
    const Level* levels = side_levels(instrument, side);
    const std::size_t count = instruments_[instrument].level_counts[side_index(side)];
    const std::size_t position = level_position(levels, count, side, price);
    return position < count && levels[position].price == price ? &levels[position] : nullptr;
}

void ItchBookBuilder::insert_order(std::uint32_t slot) noexcept {
    OrderSlot& order = orders_[slot];
    Level* levels = side_levels(order.instrument, order.side);
    std::size_t& count = instruments_[order.instrument].level_counts[side_index(order.side)];
    const std::size_t position = level_position(levels, count, order.side, order.price);
    if (position == count || levels[position].price != order.price) {
        for (std::size_t index = count; index > position; --index) levels[index] = levels[index - 1];
        levels[position] = Level{order.price, 0, 0, kNoOrderSlot, kNoOrderSlot};
        ++count;
    }
    Level& level = levels[position];
    order.previous = level.tail;
    order.next = kNoOrderSlot;
    if (level.tail == kNoOrderSlot) {
        level.head = slot;
    } else {
        orders_[level.tail].next = slot;
    }
    level.tail = slot;
    level.shares += order.shares;
    ++level.order_count;
}

void ItchBookBuilder::remove_order(std::uint32_t slot) noexcept {
    OrderSlot& order = orders_[slot];
    Level* levels = side_levels(order.instrument, order.side);
    std::size_t& count = instruments_[order.instrument].level_counts[side_index(order.side)];
    const std::size_t position = level_position(levels, count, order.side, order.price);
    Level& level = levels[position];
    if (order.previous == kNoOrderSlot) {
        level.head = order.next;
    } else {
        orders_[order.previous].next = order.next;
    }
    if (order.next == kNoOrderSlot) {
        level.tail = order.previous;
    } else {
        orders_[order.next].previous = order.previous;
    }
    level.shares -= order.shares;
    if (--level.order_count == 0) {
        for (std::size_t index = position; index + 1 < count; ++index) levels[index] = levels[index + 1];
        --count;
    }
    (void)order_slots_by_reference_.erase(order.reference);
    order.reference = 0;
    order.next = free_order_slot_;
    free_order_slot_ = slot;
    --live_orders_;
}

// ---- queries --------------------------------------------------------------------

std::size_t ItchBookBuilder::depth(InstrumentId instrument, Side side, LevelInfo* out,
                                   std::size_t max_levels) const noexcept {
    if (!valid_locate(instrument)) return 0;
    const Level* levels = side_levels(instrument, side);
    const std::size_t count = instruments_[instrument].level_counts[side_index(side)];
    const std::size_t written = count < max_levels ? count : max_levels;
    for (std::size_t index = 0; index < written; ++index)
        out[index] = LevelInfo{levels[index].price, levels[index].shares, levels[index].order_count};
    return written;
}

std::size_t ItchBookBuilder::level_orders(InstrumentId instrument, Side side, Price price, OrderId* out,
                                          std::size_t max_orders) const noexcept {
    const Level* level = find_level(instrument, side, price);
    if (level == nullptr) return 0;
    std::size_t written = 0;
    for (std::uint32_t slot = level->head; slot != kNoOrderSlot && written < max_orders; slot = orders_[slot].next)
        out[written++] = orders_[slot].reference;
    return written;
}

std::optional<Price> ItchBookBuilder::best_bid(InstrumentId instrument) const noexcept {
    if (!valid_locate(instrument) || instruments_[instrument].level_counts[0] == 0) return std::nullopt;
    return side_levels(instrument, Side::BUY)[0].price;
}

std::optional<Price> ItchBookBuilder::best_ask(InstrumentId instrument) const noexcept {
    if (!valid_locate(instrument) || instruments_[instrument].level_counts[1] == 0) return std::nullopt;
    return side_levels(instrument, Side::SELL)[0].price;
}

std::size_t ItchBookBuilder::level_count(InstrumentId instrument, Side side) const noexcept {
    return valid_locate(instrument) ? instruments_[instrument].level_counts[side_index(side)] : 0;
}

std::optional<BookOrder> ItchBookBuilder::find_order(OrderId order_reference) const noexcept {
    const std::uint32_t* slot = order_slots_by_reference_.find(order_reference);
    if (slot == nullptr) return std::nullopt;
    const OrderSlot& order = orders_[*slot];
    return BookOrder{order.reference, order.instrument, order.side, order.price, order.shares};
}

std::size_t ItchBookBuilder::order_count() const noexcept { return live_orders_; }

InstrumentId ItchBookBuilder::find_instrument(const Symbol& symbol) const noexcept {
    for (std::size_t locate = 1; locate <= max_instruments_; ++locate)
        if (instruments_[locate].directory_seen && instruments_[locate].symbol == symbol)
            return static_cast<InstrumentId>(locate);
    return 0;
}

const Symbol* ItchBookBuilder::symbol(InstrumentId instrument) const noexcept {
    return valid_locate(instrument) && instruments_[instrument].directory_seen ? &instruments_[instrument].symbol
                                                                               : nullptr;
}

char ItchBookBuilder::trading_state(InstrumentId instrument) const noexcept {
    return valid_locate(instrument) ? instruments_[instrument].trading_state : ' ';
}

std::optional<protocol::itch50::NetOrderImbalance> ItchBookBuilder::last_imbalance(
    InstrumentId instrument) const noexcept {
    if (!valid_locate(instrument) || !instruments_[instrument].imbalance_seen) return std::nullopt;
    return instruments_[instrument].imbalance;
}

std::optional<protocol::itch50::CrossTrade> ItchBookBuilder::last_cross(InstrumentId instrument) const noexcept {
    if (!valid_locate(instrument) || !instruments_[instrument].cross_seen) return std::nullopt;
    return instruments_[instrument].cross;
}

}  // namespace exsim::md
