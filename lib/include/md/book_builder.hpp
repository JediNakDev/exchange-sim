#ifndef EXSIM_MD_BOOK_BUILDER_HPP
#define EXSIM_MD_BOOK_BUILDER_HPP

// ItchBookBuilder: client-side order book rebuilt purely from ITCH 5.0
// (A/F/E/C/X/D/U add or change orders; Q/P/H/I/R/S are recorded). It never
// matches, so crossed books during call phases (and a Halted book inherited
// from one) are represented as they are.
//
// Queries mirror exsim::MatchingEngine: depth() fills LevelInfo in priority
// order (best first) and level_orders() lists OrderIds (= ITCH order
// reference numbers) in FIFO order. FIFO follows ITCH semantics: A, F and the
// new reference of U join the tail of their level; E, C and X keep position.
//
// It is also a MoldUdp64Receiver handler (on_message). All storage is sized
// at construction; apply() never allocates. A message that would make the
// book inconsistent (unknown/duplicate reference, too many shares, capacity)
// is rejected without changing the book, counted, and returned as a status.

#include <matching_engine.hpp>
#include <protocol/itch50.hpp>
#include <exchange/flat_hash_map.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace exsim::md {

struct BookBuilderConfig {
    std::size_t max_instruments = 64;           // highest stock locate accepted
    std::size_t max_orders = 1u << 20;          // live orders across all instruments
    std::size_t max_levels_per_side = 4096;     // price levels per instrument side
};

enum class BookUpdateStatus : std::uint8_t {
    Applied = 0,
    Malformed,           // decode failure or a zero share/reference field
    UnsupportedType,     // not an ITCH type this builder understands
    UnknownInstrument,   // stock locate 0 or above max_instruments
    InstrumentMismatch,  // locate differs from the referenced order's locate
    DuplicateOrder,      // reference already live
    UnknownOrder,        // reference not live
    SharesExceedOrder,   // execution/cancel larger than the order's shares
    CapacityExceeded,    // order or level storage full
};

struct BookOrder {
    OrderId order_reference = 0;
    InstrumentId instrument = 0;
    Side side = Side::BUY;
    Price price = 0;
    Quantity shares = 0;
};

class ItchBookBuilder {
public:
    // Setup only: allocates order, level and instrument storage.
    explicit ItchBookBuilder(const BookBuilderConfig& config = {});
    ~ItchBookBuilder();
    ItchBookBuilder(const ItchBookBuilder&) = delete;
    ItchBookBuilder& operator=(const ItchBookBuilder&) = delete;

    // MoldUdp64Receiver handler interface.
    void on_message(std::uint64_t sequence, std::span<const char> message) noexcept;

    // Decodes one ITCH message and applies it.
    BookUpdateStatus apply(std::span<const char> message) noexcept;

    BookUpdateStatus apply(const protocol::itch50::SystemEvent& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::StockDirectory& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::StockTradingAction& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::AddOrder& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::AddOrderMPID& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::OrderExecuted& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::OrderExecutedWithPrice& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::OrderCancel& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::OrderDelete& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::OrderReplace& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::Trade& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::CrossTrade& message) noexcept;
    BookUpdateStatus apply(const protocol::itch50::NetOrderImbalance& message) noexcept;

    // ---- queries (same shape as MatchingEngine) -----------------------------
    std::size_t depth(InstrumentId instrument, Side side, LevelInfo* out, std::size_t max_levels) const noexcept;
    std::size_t level_orders(InstrumentId instrument, Side side, Price price, OrderId* out,
                             std::size_t max_orders) const noexcept;
    [[nodiscard]] std::optional<Price> best_bid(InstrumentId instrument) const noexcept;
    [[nodiscard]] std::optional<Price> best_ask(InstrumentId instrument) const noexcept;
    [[nodiscard]] std::size_t level_count(InstrumentId instrument, Side side) const noexcept;
    [[nodiscard]] std::optional<BookOrder> find_order(OrderId order_reference) const noexcept;
    [[nodiscard]] std::size_t order_count() const noexcept;

    // Instrument metadata seen on the feed.
    [[nodiscard]] InstrumentId find_instrument(const Symbol& symbol) const noexcept;  // 0 if no R seen
    [[nodiscard]] const Symbol* symbol(InstrumentId instrument) const noexcept;       // nullptr if no R seen
    [[nodiscard]] char trading_state(InstrumentId instrument) const noexcept;         // ' ' if no H seen
    [[nodiscard]] std::optional<protocol::itch50::NetOrderImbalance> last_imbalance(
        InstrumentId instrument) const noexcept;
    [[nodiscard]] std::optional<protocol::itch50::CrossTrade> last_cross(InstrumentId instrument) const noexcept;
    [[nodiscard]] char last_system_event() const noexcept { return last_system_event_; }  // ' ' if none
    [[nodiscard]] std::size_t max_instruments() const noexcept { return max_instruments_; }

    [[nodiscard]] std::uint64_t applied_message_count() const noexcept { return applied_messages_; }
    [[nodiscard]] std::uint64_t rejected_message_count() const noexcept { return rejected_messages_; }
    [[nodiscard]] BookUpdateStatus last_rejection() const noexcept { return last_rejection_; }

private:
    static constexpr std::uint32_t kNoOrderSlot = 0xFFFFFFFFu;

    struct OrderSlot {
        OrderId reference;
        Price price;
        Quantity shares;
        std::uint32_t previous;  // within the level FIFO; next doubles as the free-list link
        std::uint32_t next;
        InstrumentId instrument;
        Side side;
    };

    struct Level {
        Price price;
        std::uint64_t shares;
        std::uint32_t order_count;
        std::uint32_t head;
        std::uint32_t tail;
    };

    struct InstrumentRecord {
        Symbol symbol{};
        protocol::itch50::NetOrderImbalance imbalance{};
        protocol::itch50::CrossTrade cross{};
        std::size_t level_counts[2] = {0, 0};  // indexed by side
        char trading_state = ' ';
        bool directory_seen = false;
        bool imbalance_seen = false;
        bool cross_seen = false;
    };

    bool valid_locate(InstrumentId instrument) const noexcept {
        return instrument != 0 && instrument <= max_instruments_;
    }
    Level* side_levels(InstrumentId instrument, Side side) noexcept;
    const Level* side_levels(InstrumentId instrument, Side side) const noexcept;
    // Index of the first level whose price is not better than `price`.
    std::size_t level_position(const Level* levels, std::size_t count, Side side, Price price) const noexcept;
    const Level* find_level(InstrumentId instrument, Side side, Price price) const noexcept;

    BookUpdateStatus add_order(InstrumentId instrument, OrderId reference, Side side, Price price,
                               Quantity shares) noexcept;
    BookUpdateStatus reduce_order(InstrumentId instrument, OrderId reference, Quantity shares) noexcept;
    BookUpdateStatus delete_order(InstrumentId instrument, OrderId reference) noexcept;
    void insert_order(std::uint32_t slot) noexcept;  // level must have room (checked by callers)
    void remove_order(std::uint32_t slot) noexcept;
    BookUpdateStatus record(BookUpdateStatus status) noexcept;
    template <class Message>
    BookUpdateStatus decode_then_apply(std::span<const char> bytes) noexcept;

    std::size_t max_instruments_;
    std::size_t max_orders_;
    std::size_t max_levels_per_side_;
    std::unique_ptr<OrderSlot[]> orders_;
    std::unique_ptr<Level[]> levels_;
    std::unique_ptr<InstrumentRecord[]> instruments_;
    lob::FlatHashMap<std::uint32_t> order_slots_by_reference_;
    std::uint32_t free_order_slot_ = kNoOrderSlot;
    std::size_t live_orders_ = 0;
    std::uint64_t applied_messages_ = 0;
    std::uint64_t rejected_messages_ = 0;
    BookUpdateStatus last_rejection_ = BookUpdateStatus::Applied;
    char last_system_event_ = ' ';
};

}  // namespace exsim::md

#endif
