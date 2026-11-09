#ifndef EXSIM_PROTOCOL_SBE_ORDER_ENTRY_HPP
#define EXSIM_PROTOCOL_SBE_ORDER_ENTRY_HPP
// Hand-written codec for lib/include/protocol/fixp_sbe_format.xml (schema id
// 22597, version 1): SBE 1.0 little-endian messages framed by SOFH 1.0.
//
//   SOFH (6 bytes, big-endian) | SBE message header (12 bytes) | root block |
//   optional variable length field (uint16 length + bytes)
//
// References: SBE 1.0 (2020 errata) 4.4 integers, 4.5 decimals with constant
// exponent, 4.7.2 NUL padded char arrays, 4.8.2 variable length data, 4.10.3
// UTCTimestamp, 4.13 enums, 4.14 sets, 5.1.1 framing, 5.2.1 message header,
// 5.3.3.1 no padding, 7.3.1 and 7.4 extension; SOFH 1.0 Message_Length and
// Encoding_Type fields; FIXP 1.0 section 6 session message summary.
//
// Decoding is two-step and allocation free: decode_frame validates the SOFH and
// message header of one complete frame and returns a FrameView; decode(view,
// message) checks the template id, uses the header blockLength to skip unknown
// trailing root block bytes of a newer schema version and reads the fields.
// Variable length fields decode to std::string_view into the caller's buffer.
// Enum fields are decoded raw (any underlying value); the gateway validates.
// encode(message, out, capacity) writes one whole frame and returns its size,
// or 0 when the capacity or a variable length field is too large.
#include <array>
#include <cstddef>
#include <cstdint>
#include <instrument.hpp>
#include <protocol/wire.hpp>
#include <types.hpp>
#include <limits>
#include <string_view>
#include <type_traits>

namespace exsim::protocol::sbe_order_entry {

inline constexpr std::uint16_t kSchemaId = 22597;
inline constexpr std::uint16_t kSchemaVersion = 1;
inline constexpr std::uint16_t kSofhEncodingTypeSbeLittleEndian = 0xEB50;
inline constexpr std::size_t kSofhSize = 6;
inline constexpr std::size_t kMessageHeaderSize = 12;
inline constexpr std::size_t kFramePrefixSize = kSofhSize + kMessageHeaderSize;
inline constexpr std::size_t kVariableLengthPrefixSize = 2;
inline constexpr std::size_t kMaxVariableLengthFieldBytes = 128;
// Largest frame version 1 produces (Establish with full credentials).
inline constexpr std::size_t kMaxEncodedFrameSize = kFramePrefixSize + 36 + kVariableLengthPrefixSize + kMaxVariableLengthFieldBytes;
// Largest frame accepted from a peer; leaves room for appended fields of later versions.
inline constexpr std::size_t kMaxAcceptedFrameSize = 1024;
inline constexpr std::uint64_t kNullSequenceNumber = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::int64_t kNullPriceMantissa = std::numeric_limits<std::int64_t>::min();
inline constexpr std::uint8_t kExecInstParticipateDontInitiate = 1u << 0;
// Template ids 1..99 belong to the FIXP session layer.
inline constexpr std::uint16_t kFirstApplicationTemplateId = 100;

enum class TemplateId : std::uint16_t {
    Negotiate = 1,
    NegotiationResponse = 2,
    NegotiationReject = 3,
    Establish = 5,
    EstablishmentAck = 6,
    EstablishmentReject = 7,
    Sequence = 8,
    UnsequencedHeartbeat = 10,
    RetransmitRequest = 11,
    Retransmission = 12,
    RetransmitReject = 13,
    Terminate = 14,
    NotApplied = 18,
    NewOrderSingle = 100,
    OrderCancelRequest = 101,
    OrderCancelReplaceRequest = 102,
    OrderMassCancelRequest = 103,
    ExecutionReportNew = 200,
    ExecutionReportTrade = 201,
    ExecutionReportCancel = 202,
    ExecutionReportReplace = 203,
    ExecutionReportReject = 204,
    OrderCancelReject = 205,
    OrderMassCancelReport = 206,
    BusinessMessageReject = 207,
};

enum class DecodeStatus : std::uint8_t {
    Ok,
    Incomplete,                        // need more bytes for one frame
    FrameLengthTooSmall,               // SOFH length below SOFH + message header
    FrameLengthTooLarge,               // SOFH length above kMaxAcceptedFrameSize
    UnsupportedEncodingType,           // SOFH encoding type is not 0xEB50
    UnknownSchemaId,
    UnsupportedSchemaVersion,          // header version below 1
    BlockExceedsFrame,                 // blockLength runs past the SOFH length
    TemplateMismatch,                  // decode called with the wrong message type
    BlockLengthTooShort,               // blockLength below this version's root block
    UnexpectedRepeatingGroups,         // numGroups != 0
    VariableLengthFieldCountMismatch,  // numVarDataFields wrong for the template
    VariableLengthFieldOverrun,        // length prefix runs past the frame
    VariableLengthFieldTooLong,        // above kMaxVariableLengthFieldBytes
    TrailingBytes,                     // version 1 frame longer than its content
};

[[nodiscard]] constexpr std::uint16_t template_id_value(TemplateId template_id) noexcept {
    return static_cast<std::uint16_t>(template_id);
}

// ---------------------------------------------------------------------------
// Explicit little-endian codecs (SBE 4.4.3). Callers bounds-check.
namespace little_endian {
template <unsigned ByteCount>
constexpr std::uint64_t load(const char* bytes) noexcept {
    std::uint64_t value = 0;
    for (unsigned index = ByteCount; index > 0; --index) {
        value = (value << 8) | static_cast<unsigned char>(bytes[index - 1]);
    }
    return value;
}
template <unsigned ByteCount>
constexpr void store(char* bytes, std::uint64_t value) noexcept {
    for (unsigned index = 0; index < ByteCount; ++index) {
        bytes[index] = static_cast<char>(value & 0xFF);
        value >>= 8;
    }
}
constexpr std::uint8_t load8(const char* bytes) noexcept { return static_cast<std::uint8_t>(bytes[0]); }
constexpr std::uint16_t load16(const char* bytes) noexcept { return static_cast<std::uint16_t>(load<2>(bytes)); }
constexpr std::uint32_t load32(const char* bytes) noexcept { return static_cast<std::uint32_t>(load<4>(bytes)); }
constexpr std::uint64_t load64(const char* bytes) noexcept { return load<8>(bytes); }
constexpr std::int64_t load_signed64(const char* bytes) noexcept { return static_cast<std::int64_t>(load<8>(bytes)); }
constexpr void store8(char* bytes, std::uint8_t value) noexcept { bytes[0] = static_cast<char>(value); }
constexpr void store16(char* bytes, std::uint16_t value) noexcept { store<2>(bytes, value); }
constexpr void store32(char* bytes, std::uint32_t value) noexcept { store<4>(bytes, value); }
constexpr void store64(char* bytes, std::uint64_t value) noexcept { store<8>(bytes, value); }
constexpr void store_signed64(char* bytes, std::int64_t value) noexcept { store<8>(bytes, static_cast<std::uint64_t>(value)); }
}  // namespace little_endian

// ---------------------------------------------------------------------------
// Field types.

using SessionUuid = std::array<std::uint8_t, 16>;
using WireSymbol = std::array<char, 8>;  // NUL padded (SBE 4.7.2)

// RFC 4122 version 4 (random) UUID: version nibble 4, variant bits 10.
[[nodiscard]] constexpr bool is_version4_uuid(const SessionUuid& uuid) noexcept {
    return (uuid[6] & 0xF0) == 0x40 && (uuid[8] & 0xC0) == 0x80;
}

// exsim symbols are space padded; the wire form is NUL padded.
[[nodiscard]] inline WireSymbol wire_symbol_from(const Symbol& symbol) noexcept {
    WireSymbol wire{};
    const std::string_view text = symbol.view();
    for (std::size_t index = 0; index < text.size(); ++index) wire[index] = text[index];
    return wire;
}
// Accepts NUL or space padding; false for empty symbols and embedded padding.
[[nodiscard]] constexpr bool exsim_symbol_from_wire(const WireSymbol& wire, Symbol& symbol) noexcept {
    std::size_t length = 0;
    while (length < wire.size() && wire[length] != '\0' && wire[length] != ' ') ++length;
    if (length == 0) return false;
    for (std::size_t index = length; index < wire.size(); ++index) {
        if (wire[index] != '\0' && wire[index] != ' ') return false;
    }
    symbol = Symbol(std::string_view(wire.data(), length));
    return true;
}

enum class FlowType : std::uint8_t { Recoverable = 0, Idempotent = 1, Unsequenced = 2, None = 3 };
enum class NegotiationRejectCode : std::uint8_t { Credentials = 0, FlowTypeNotSupported = 1, DuplicateId = 2, Unspecified = 3 };
enum class EstablishmentRejectCode : std::uint8_t {
    Unnegotiated = 0,
    AlreadyEstablished = 1,
    SessionBlocked = 2,
    KeepaliveInterval = 3,
    Credentials = 4,
    Unspecified = 5,
};
enum class RetransmitRejectCode : std::uint8_t { OutOfRange = 0, InvalidSession = 1, RequestLimitExceeded = 2 };
enum class TerminationCode : std::uint8_t { Finished = 0, UnspecifiedError = 1, ReRequestOutOfBounds = 2, ReRequestInProgress = 3 };

enum class SideCode : char { Buy = '1', Sell = '2' };
enum class OrdTypeCode : char { Market = '1', Limit = '2' };
enum class TimeInForceCode : char { Day = '0', GoodTillCancel = '1', ImmediateOrCancel = '3', FillOrKill = '4' };
enum class SelfTradePreventionCode : char {
    UseUserDefault = '\0',
    None = 'N',
    CancelResting = 'R',
    CancelAggressor = 'A',
    CancelBoth = 'B',
};
enum class OrdStatusCode : char { New = '0', PartiallyFilled = '1', Filled = '2', Canceled = '4', Rejected = '8' };
enum class LastLiquidityIndicator : std::uint8_t { AddedLiquidity = 1, RemovedLiquidity = 2, Auction = 4 };
enum class BooleanCode : std::uint8_t { False = 0, True = 1 };
enum class OrdRejReasonCode : std::uint8_t { UnknownSymbol = 1, ExchangeClosed = 2, UnknownOrder = 5, DuplicateOrder = 6, Other = 99 };
enum class CxlRejResponseToCode : char { OrderCancelRequest = '1', OrderCancelReplaceRequest = '2' };
enum class CxlRejReasonCode : std::uint8_t { UnknownOrder = 1, DuplicateClOrdId = 6, Other = 99 };
enum class MassCancelRequestTypeCode : char { CancelOrdersForSecurity = '1', CancelAllOrders = '7' };
enum class MassCancelResponseCode : char { CancelRequestRejected = '0', CancelOrdersForSecurity = '1', CancelAllOrders = '7' };
enum class MassCancelRejectReasonCode : std::uint8_t {
    MassCancelNotSupported = 0,
    InvalidOrUnknownSecurity = 1,
    Other = 99,
    NullValue = 255,
};
enum class BusinessRejectReasonCode : std::uint8_t { Other = 0, UnsupportedMessageType = 3 };
// Values equal exsim::CancelReason.
enum class ExsimCancelReasonCode : std::uint8_t {
    UserRequested = 0,
    ImmediateOrCancel = 1,
    FillOrKill = 2,
    SelfTradePrevention = 3,
    MassCancel = 4,
    CancelOnDisconnect = 5,
    EndOfDay = 6,
};
// 1..12 equal exsim::RejectReason; 64 and above are gateway reasons.
enum class ExsimRejectReasonCode : std::uint8_t {
    None = 0,
    UnknownInstrument = 1,
    InvalidClOrdId = 2,
    DuplicateClOrdId = 3,
    InvalidQuantity = 4,
    InvalidPrice = 5,
    InvalidField = 6,
    MarketClosed = 7,
    InstrumentHalted = 8,
    NotAllowedInPhase = 9,
    PostOnlyWouldCross = 10,
    CapacityExceeded = 11,
    UnknownOrder = 12,
    ClOrdIdAlreadyUsedToday = 64,
    TokenTableFull = 65,
    OutboundStoreFull = 66,
    InjectedReject = 67,
    RequestHadNoEffect = 69,
};

// ---------------------------------------------------------------------------
// Messages. kBlockLength is the version 1 root block size in bytes.

struct Negotiate {
    static constexpr TemplateId kTemplateId = TemplateId::Negotiate;
    static constexpr std::uint16_t kBlockLength = 25;
    static constexpr bool kHasVariableLengthField = true;
    SessionUuid session_id{};
    std::uint64_t timestamp_ns = 0;
    FlowType client_flow = FlowType::Idempotent;
    std::string_view credentials;
};
struct NegotiationResponse {
    static constexpr TemplateId kTemplateId = TemplateId::NegotiationResponse;
    static constexpr std::uint16_t kBlockLength = 25;
    static constexpr bool kHasVariableLengthField = true;
    SessionUuid session_id{};
    std::uint64_t request_timestamp_ns = 0;
    FlowType server_flow = FlowType::Recoverable;
    std::string_view credentials;
};
struct NegotiationReject {
    static constexpr TemplateId kTemplateId = TemplateId::NegotiationReject;
    static constexpr std::uint16_t kBlockLength = 25;
    static constexpr bool kHasVariableLengthField = true;
    SessionUuid session_id{};
    std::uint64_t request_timestamp_ns = 0;
    NegotiationRejectCode code = NegotiationRejectCode::Unspecified;
    std::string_view reason;
};
struct Establish {
    static constexpr TemplateId kTemplateId = TemplateId::Establish;
    static constexpr std::uint16_t kBlockLength = 36;
    static constexpr bool kHasVariableLengthField = true;
    SessionUuid session_id{};
    std::uint64_t timestamp_ns = 0;
    std::uint32_t keepalive_interval_ms = 0;
    std::uint64_t next_seq_no = kNullSequenceNumber;
    std::string_view credentials;
};
struct EstablishmentAck {
    static constexpr TemplateId kTemplateId = TemplateId::EstablishmentAck;
    static constexpr std::uint16_t kBlockLength = 36;
    static constexpr bool kHasVariableLengthField = false;
    SessionUuid session_id{};
    std::uint64_t request_timestamp_ns = 0;
    std::uint32_t keepalive_interval_ms = 0;
    std::uint64_t next_seq_no = kNullSequenceNumber;
};
struct EstablishmentReject {
    static constexpr TemplateId kTemplateId = TemplateId::EstablishmentReject;
    static constexpr std::uint16_t kBlockLength = 25;
    static constexpr bool kHasVariableLengthField = true;
    SessionUuid session_id{};
    std::uint64_t request_timestamp_ns = 0;
    EstablishmentRejectCode code = EstablishmentRejectCode::Unspecified;
    std::string_view reason;
};
struct Sequence {
    static constexpr TemplateId kTemplateId = TemplateId::Sequence;
    static constexpr std::uint16_t kBlockLength = 8;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t next_seq_no = 0;
};
struct UnsequencedHeartbeat {
    static constexpr TemplateId kTemplateId = TemplateId::UnsequencedHeartbeat;
    static constexpr std::uint16_t kBlockLength = 0;
    static constexpr bool kHasVariableLengthField = false;
};
struct RetransmitRequest {
    static constexpr TemplateId kTemplateId = TemplateId::RetransmitRequest;
    static constexpr std::uint16_t kBlockLength = 36;
    static constexpr bool kHasVariableLengthField = false;
    SessionUuid session_id{};
    std::uint64_t timestamp_ns = 0;
    std::uint64_t from_seq_no = 0;
    std::uint32_t count = 0;
};
struct Retransmission {
    static constexpr TemplateId kTemplateId = TemplateId::Retransmission;
    static constexpr std::uint16_t kBlockLength = 36;
    static constexpr bool kHasVariableLengthField = false;
    SessionUuid session_id{};
    std::uint64_t request_timestamp_ns = 0;
    std::uint64_t next_seq_no = 0;
    std::uint32_t count = 0;
};
struct RetransmitReject {
    static constexpr TemplateId kTemplateId = TemplateId::RetransmitReject;
    static constexpr std::uint16_t kBlockLength = 25;
    static constexpr bool kHasVariableLengthField = true;
    SessionUuid session_id{};
    std::uint64_t request_timestamp_ns = 0;
    RetransmitRejectCode code = RetransmitRejectCode::OutOfRange;
    std::string_view reason;
};
struct Terminate {
    static constexpr TemplateId kTemplateId = TemplateId::Terminate;
    static constexpr std::uint16_t kBlockLength = 17;
    static constexpr bool kHasVariableLengthField = true;
    SessionUuid session_id{};
    TerminationCode code = TerminationCode::Finished;
    std::string_view reason;
};
struct NotApplied {
    static constexpr TemplateId kTemplateId = TemplateId::NotApplied;
    static constexpr std::uint16_t kBlockLength = 12;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t from_seq_no = 0;
    std::uint32_t count = 0;
};

struct NewOrderSingle {
    static constexpr TemplateId kTemplateId = TemplateId::NewOrderSingle;
    static constexpr std::uint16_t kBlockLength = 33;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t cl_ord_id = 0;
    std::int64_t price_mantissa = kNullPriceMantissa;
    std::uint32_t order_qty = 0;
    WireSymbol symbol{};
    SideCode side = SideCode::Buy;
    OrdTypeCode ord_type = OrdTypeCode::Limit;
    TimeInForceCode time_in_force = TimeInForceCode::Day;
    std::uint8_t exec_inst = 0;
    SelfTradePreventionCode self_trade_prevention = SelfTradePreventionCode::UseUserDefault;
};
struct OrderCancelRequest {
    static constexpr TemplateId kTemplateId = TemplateId::OrderCancelRequest;
    static constexpr std::uint16_t kBlockLength = 20;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t orig_cl_ord_id = 0;
    std::uint32_t reduce_to_total_qty = 0;  // 0 = cancel fully
};
struct OrderCancelReplaceRequest {
    static constexpr TemplateId kTemplateId = TemplateId::OrderCancelReplaceRequest;
    static constexpr std::uint16_t kBlockLength = 29;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t orig_cl_ord_id = 0;
    std::int64_t price_mantissa = 0;
    std::uint32_t order_qty = 0;  // new TOTAL quantity
    BooleanCode force_priority_loss = BooleanCode::False;
};
struct OrderMassCancelRequest {
    static constexpr TemplateId kTemplateId = TemplateId::OrderMassCancelRequest;
    static constexpr std::uint16_t kBlockLength = 17;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t cl_ord_id = 0;
    WireSymbol symbol{};
    MassCancelRequestTypeCode mass_cancel_request_type = MassCancelRequestTypeCode::CancelAllOrders;
};

struct ExecutionReportNew {
    static constexpr TemplateId kTemplateId = TemplateId::ExecutionReportNew;
    static constexpr std::uint16_t kBlockLength = 66;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t order_id = 0;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t exec_id = 0;
    std::uint64_t transact_time_ns = 0;
    std::int64_t price_mantissa = kNullPriceMantissa;
    std::uint32_t order_qty = 0;
    std::uint32_t leaves_qty = 0;
    std::uint32_t cum_qty = 0;
    WireSymbol symbol{};
    SideCode side = SideCode::Buy;
    OrdTypeCode ord_type = OrdTypeCode::Limit;
    TimeInForceCode time_in_force = TimeInForceCode::Day;
    OrdStatusCode ord_status = OrdStatusCode::New;
    std::uint8_t exec_inst = 0;
    SelfTradePreventionCode self_trade_prevention = SelfTradePreventionCode::None;
};
struct ExecutionReportTrade {
    static constexpr TemplateId kTemplateId = TemplateId::ExecutionReportTrade;
    static constexpr std::uint16_t kBlockLength = 71;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t order_id = 0;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t exec_id = 0;
    std::uint64_t transact_time_ns = 0;
    std::int64_t last_px_mantissa = 0;
    std::uint64_t trd_match_id = 0;
    std::uint32_t last_qty = 0;
    std::uint32_t leaves_qty = 0;
    std::uint32_t cum_qty = 0;
    WireSymbol symbol{};
    SideCode side = SideCode::Buy;
    OrdStatusCode ord_status = OrdStatusCode::PartiallyFilled;
    LastLiquidityIndicator last_liquidity_ind = LastLiquidityIndicator::AddedLiquidity;
};
struct ExecutionReportCancel {
    static constexpr TemplateId kTemplateId = TemplateId::ExecutionReportCancel;
    static constexpr std::uint16_t kBlockLength = 63;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t order_id = 0;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t orig_cl_ord_id = 0;
    std::uint64_t exec_id = 0;
    std::uint64_t transact_time_ns = 0;
    std::uint32_t canceled_qty = 0;
    std::uint32_t leaves_qty = 0;
    std::uint32_t cum_qty = 0;
    WireSymbol symbol{};
    SideCode side = SideCode::Buy;
    OrdStatusCode ord_status = OrdStatusCode::Canceled;
    ExsimCancelReasonCode cancel_reason = ExsimCancelReasonCode::UserRequested;
};
struct ExecutionReportReplace {
    static constexpr TemplateId kTemplateId = TemplateId::ExecutionReportReplace;
    static constexpr std::uint16_t kBlockLength = 79;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t order_id = 0;
    std::uint64_t previous_order_id = 0;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t orig_cl_ord_id = 0;
    std::uint64_t exec_id = 0;
    std::uint64_t transact_time_ns = 0;
    std::int64_t price_mantissa = 0;
    std::uint32_t order_qty = 0;
    std::uint32_t leaves_qty = 0;
    std::uint32_t cum_qty = 0;
    WireSymbol symbol{};
    SideCode side = SideCode::Buy;
    OrdStatusCode ord_status = OrdStatusCode::New;
    BooleanCode priority_retained = BooleanCode::False;
};
struct ExecutionReportReject {
    static constexpr TemplateId kTemplateId = TemplateId::ExecutionReportReject;
    static constexpr std::uint16_t kBlockLength = 34;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t exec_id = 0;
    std::uint64_t transact_time_ns = 0;
    WireSymbol symbol{};
    OrdRejReasonCode ord_rej_reason = OrdRejReasonCode::Other;
    ExsimRejectReasonCode exsim_reject_reason = ExsimRejectReasonCode::None;
};
struct OrderCancelReject {
    static constexpr TemplateId kTemplateId = TemplateId::OrderCancelReject;
    static constexpr std::uint16_t kBlockLength = 35;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t orig_cl_ord_id = 0;
    std::uint64_t order_id = 0;
    std::uint64_t transact_time_ns = 0;
    CxlRejResponseToCode cxl_rej_response_to = CxlRejResponseToCode::OrderCancelRequest;
    CxlRejReasonCode cxl_rej_reason = CxlRejReasonCode::Other;
    ExsimRejectReasonCode exsim_reject_reason = ExsimRejectReasonCode::None;
};
struct OrderMassCancelReport {
    static constexpr TemplateId kTemplateId = TemplateId::OrderMassCancelReport;
    static constexpr std::uint16_t kBlockLength = 32;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t cl_ord_id = 0;
    std::uint64_t transact_time_ns = 0;
    std::uint32_t total_affected_orders = 0;
    WireSymbol symbol{};
    MassCancelRequestTypeCode mass_cancel_request_type = MassCancelRequestTypeCode::CancelAllOrders;
    MassCancelResponseCode mass_cancel_response = MassCancelResponseCode::CancelAllOrders;
    MassCancelRejectReasonCode mass_cancel_reject_reason = MassCancelRejectReasonCode::NullValue;
    ExsimRejectReasonCode exsim_reject_reason = ExsimRejectReasonCode::None;
};
struct BusinessMessageReject {
    static constexpr TemplateId kTemplateId = TemplateId::BusinessMessageReject;
    static constexpr std::uint16_t kBlockLength = 11;
    static constexpr bool kHasVariableLengthField = false;
    std::uint64_t ref_seq_num = 0;
    std::uint16_t ref_msg_type = 0;
    BusinessRejectReasonCode business_reject_reason = BusinessRejectReasonCode::UnsupportedMessageType;
};

// ---------------------------------------------------------------------------
// Root block layouts (offsets from the start of the root block).

namespace detail {
namespace le = little_endian;

template <typename Enum>
constexpr void store_enum(char* bytes, Enum value) noexcept {
    bytes[0] = static_cast<char>(static_cast<std::underlying_type_t<Enum>>(value));
}
template <typename Enum>
constexpr Enum load_enum(const char* bytes) noexcept {
    return static_cast<Enum>(static_cast<std::underlying_type_t<Enum>>(bytes[0]));
}
constexpr void store_uuid(char* bytes, const SessionUuid& uuid) noexcept {
    for (std::size_t index = 0; index < uuid.size(); ++index) bytes[index] = static_cast<char>(uuid[index]);
}
constexpr void load_uuid(const char* bytes, SessionUuid& uuid) noexcept {
    for (std::size_t index = 0; index < uuid.size(); ++index) uuid[index] = static_cast<std::uint8_t>(bytes[index]);
}
constexpr void store_symbol(char* bytes, const WireSymbol& symbol) noexcept {
    for (std::size_t index = 0; index < symbol.size(); ++index) bytes[index] = symbol[index];
}
constexpr void load_symbol(const char* bytes, WireSymbol& symbol) noexcept {
    for (std::size_t index = 0; index < symbol.size(); ++index) symbol[index] = bytes[index];
}
}  // namespace detail

inline void write_root_block(const Negotiate& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.timestamp_ns);
    detail::store_enum(block + 24, message.client_flow);
}
inline void read_root_block(const char* block, Negotiate& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.timestamp_ns = little_endian::load64(block + 16);
    message.client_flow = detail::load_enum<FlowType>(block + 24);
}
inline std::string_view variable_length_field(const Negotiate& message) noexcept { return message.credentials; }
inline void set_variable_length_field(Negotiate& message, std::string_view bytes) noexcept { message.credentials = bytes; }

inline void write_root_block(const NegotiationResponse& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.request_timestamp_ns);
    detail::store_enum(block + 24, message.server_flow);
}
inline void read_root_block(const char* block, NegotiationResponse& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.request_timestamp_ns = little_endian::load64(block + 16);
    message.server_flow = detail::load_enum<FlowType>(block + 24);
}
inline std::string_view variable_length_field(const NegotiationResponse& message) noexcept { return message.credentials; }
inline void set_variable_length_field(NegotiationResponse& message, std::string_view bytes) noexcept { message.credentials = bytes; }

inline void write_root_block(const NegotiationReject& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.request_timestamp_ns);
    detail::store_enum(block + 24, message.code);
}
inline void read_root_block(const char* block, NegotiationReject& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.request_timestamp_ns = little_endian::load64(block + 16);
    message.code = detail::load_enum<NegotiationRejectCode>(block + 24);
}
inline std::string_view variable_length_field(const NegotiationReject& message) noexcept { return message.reason; }
inline void set_variable_length_field(NegotiationReject& message, std::string_view bytes) noexcept { message.reason = bytes; }

inline void write_root_block(const Establish& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.timestamp_ns);
    little_endian::store32(block + 24, message.keepalive_interval_ms);
    little_endian::store64(block + 28, message.next_seq_no);
}
inline void read_root_block(const char* block, Establish& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.timestamp_ns = little_endian::load64(block + 16);
    message.keepalive_interval_ms = little_endian::load32(block + 24);
    message.next_seq_no = little_endian::load64(block + 28);
}
inline std::string_view variable_length_field(const Establish& message) noexcept { return message.credentials; }
inline void set_variable_length_field(Establish& message, std::string_view bytes) noexcept { message.credentials = bytes; }

inline void write_root_block(const EstablishmentAck& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.request_timestamp_ns);
    little_endian::store32(block + 24, message.keepalive_interval_ms);
    little_endian::store64(block + 28, message.next_seq_no);
}
inline void read_root_block(const char* block, EstablishmentAck& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.request_timestamp_ns = little_endian::load64(block + 16);
    message.keepalive_interval_ms = little_endian::load32(block + 24);
    message.next_seq_no = little_endian::load64(block + 28);
}

inline void write_root_block(const EstablishmentReject& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.request_timestamp_ns);
    detail::store_enum(block + 24, message.code);
}
inline void read_root_block(const char* block, EstablishmentReject& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.request_timestamp_ns = little_endian::load64(block + 16);
    message.code = detail::load_enum<EstablishmentRejectCode>(block + 24);
}
inline std::string_view variable_length_field(const EstablishmentReject& message) noexcept { return message.reason; }
inline void set_variable_length_field(EstablishmentReject& message, std::string_view bytes) noexcept { message.reason = bytes; }

inline void write_root_block(const Sequence& message, char* block) noexcept {
    little_endian::store64(block + 0, message.next_seq_no);
}
inline void read_root_block(const char* block, Sequence& message) noexcept {
    message.next_seq_no = little_endian::load64(block + 0);
}

inline void write_root_block(const UnsequencedHeartbeat&, char*) noexcept {}
inline void read_root_block(const char*, UnsequencedHeartbeat&) noexcept {}

inline void write_root_block(const RetransmitRequest& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.timestamp_ns);
    little_endian::store64(block + 24, message.from_seq_no);
    little_endian::store32(block + 32, message.count);
}
inline void read_root_block(const char* block, RetransmitRequest& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.timestamp_ns = little_endian::load64(block + 16);
    message.from_seq_no = little_endian::load64(block + 24);
    message.count = little_endian::load32(block + 32);
}

inline void write_root_block(const Retransmission& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.request_timestamp_ns);
    little_endian::store64(block + 24, message.next_seq_no);
    little_endian::store32(block + 32, message.count);
}
inline void read_root_block(const char* block, Retransmission& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.request_timestamp_ns = little_endian::load64(block + 16);
    message.next_seq_no = little_endian::load64(block + 24);
    message.count = little_endian::load32(block + 32);
}

inline void write_root_block(const RetransmitReject& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    little_endian::store64(block + 16, message.request_timestamp_ns);
    detail::store_enum(block + 24, message.code);
}
inline void read_root_block(const char* block, RetransmitReject& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.request_timestamp_ns = little_endian::load64(block + 16);
    message.code = detail::load_enum<RetransmitRejectCode>(block + 24);
}
inline std::string_view variable_length_field(const RetransmitReject& message) noexcept { return message.reason; }
inline void set_variable_length_field(RetransmitReject& message, std::string_view bytes) noexcept { message.reason = bytes; }

inline void write_root_block(const Terminate& message, char* block) noexcept {
    detail::store_uuid(block + 0, message.session_id);
    detail::store_enum(block + 16, message.code);
}
inline void read_root_block(const char* block, Terminate& message) noexcept {
    detail::load_uuid(block + 0, message.session_id);
    message.code = detail::load_enum<TerminationCode>(block + 16);
}
inline std::string_view variable_length_field(const Terminate& message) noexcept { return message.reason; }
inline void set_variable_length_field(Terminate& message, std::string_view bytes) noexcept { message.reason = bytes; }

inline void write_root_block(const NotApplied& message, char* block) noexcept {
    little_endian::store64(block + 0, message.from_seq_no);
    little_endian::store32(block + 8, message.count);
}
inline void read_root_block(const char* block, NotApplied& message) noexcept {
    message.from_seq_no = little_endian::load64(block + 0);
    message.count = little_endian::load32(block + 8);
}

inline void write_root_block(const NewOrderSingle& message, char* block) noexcept {
    little_endian::store64(block + 0, message.cl_ord_id);
    little_endian::store_signed64(block + 8, message.price_mantissa);
    little_endian::store32(block + 16, message.order_qty);
    detail::store_symbol(block + 20, message.symbol);
    detail::store_enum(block + 28, message.side);
    detail::store_enum(block + 29, message.ord_type);
    detail::store_enum(block + 30, message.time_in_force);
    little_endian::store8(block + 31, message.exec_inst);
    detail::store_enum(block + 32, message.self_trade_prevention);
}
inline void read_root_block(const char* block, NewOrderSingle& message) noexcept {
    message.cl_ord_id = little_endian::load64(block + 0);
    message.price_mantissa = little_endian::load_signed64(block + 8);
    message.order_qty = little_endian::load32(block + 16);
    detail::load_symbol(block + 20, message.symbol);
    message.side = detail::load_enum<SideCode>(block + 28);
    message.ord_type = detail::load_enum<OrdTypeCode>(block + 29);
    message.time_in_force = detail::load_enum<TimeInForceCode>(block + 30);
    message.exec_inst = little_endian::load8(block + 31);
    message.self_trade_prevention = detail::load_enum<SelfTradePreventionCode>(block + 32);
}

inline void write_root_block(const OrderCancelRequest& message, char* block) noexcept {
    little_endian::store64(block + 0, message.cl_ord_id);
    little_endian::store64(block + 8, message.orig_cl_ord_id);
    little_endian::store32(block + 16, message.reduce_to_total_qty);
}
inline void read_root_block(const char* block, OrderCancelRequest& message) noexcept {
    message.cl_ord_id = little_endian::load64(block + 0);
    message.orig_cl_ord_id = little_endian::load64(block + 8);
    message.reduce_to_total_qty = little_endian::load32(block + 16);
}

inline void write_root_block(const OrderCancelReplaceRequest& message, char* block) noexcept {
    little_endian::store64(block + 0, message.cl_ord_id);
    little_endian::store64(block + 8, message.orig_cl_ord_id);
    little_endian::store_signed64(block + 16, message.price_mantissa);
    little_endian::store32(block + 24, message.order_qty);
    detail::store_enum(block + 28, message.force_priority_loss);
}
inline void read_root_block(const char* block, OrderCancelReplaceRequest& message) noexcept {
    message.cl_ord_id = little_endian::load64(block + 0);
    message.orig_cl_ord_id = little_endian::load64(block + 8);
    message.price_mantissa = little_endian::load_signed64(block + 16);
    message.order_qty = little_endian::load32(block + 24);
    message.force_priority_loss = detail::load_enum<BooleanCode>(block + 28);
}

inline void write_root_block(const OrderMassCancelRequest& message, char* block) noexcept {
    little_endian::store64(block + 0, message.cl_ord_id);
    detail::store_symbol(block + 8, message.symbol);
    detail::store_enum(block + 16, message.mass_cancel_request_type);
}
inline void read_root_block(const char* block, OrderMassCancelRequest& message) noexcept {
    message.cl_ord_id = little_endian::load64(block + 0);
    detail::load_symbol(block + 8, message.symbol);
    message.mass_cancel_request_type = detail::load_enum<MassCancelRequestTypeCode>(block + 16);
}

inline void write_root_block(const ExecutionReportNew& message, char* block) noexcept {
    little_endian::store64(block + 0, message.order_id);
    little_endian::store64(block + 8, message.cl_ord_id);
    little_endian::store64(block + 16, message.exec_id);
    little_endian::store64(block + 24, message.transact_time_ns);
    little_endian::store_signed64(block + 32, message.price_mantissa);
    little_endian::store32(block + 40, message.order_qty);
    little_endian::store32(block + 44, message.leaves_qty);
    little_endian::store32(block + 48, message.cum_qty);
    detail::store_symbol(block + 52, message.symbol);
    detail::store_enum(block + 60, message.side);
    detail::store_enum(block + 61, message.ord_type);
    detail::store_enum(block + 62, message.time_in_force);
    detail::store_enum(block + 63, message.ord_status);
    little_endian::store8(block + 64, message.exec_inst);
    detail::store_enum(block + 65, message.self_trade_prevention);
}
inline void read_root_block(const char* block, ExecutionReportNew& message) noexcept {
    message.order_id = little_endian::load64(block + 0);
    message.cl_ord_id = little_endian::load64(block + 8);
    message.exec_id = little_endian::load64(block + 16);
    message.transact_time_ns = little_endian::load64(block + 24);
    message.price_mantissa = little_endian::load_signed64(block + 32);
    message.order_qty = little_endian::load32(block + 40);
    message.leaves_qty = little_endian::load32(block + 44);
    message.cum_qty = little_endian::load32(block + 48);
    detail::load_symbol(block + 52, message.symbol);
    message.side = detail::load_enum<SideCode>(block + 60);
    message.ord_type = detail::load_enum<OrdTypeCode>(block + 61);
    message.time_in_force = detail::load_enum<TimeInForceCode>(block + 62);
    message.ord_status = detail::load_enum<OrdStatusCode>(block + 63);
    message.exec_inst = little_endian::load8(block + 64);
    message.self_trade_prevention = detail::load_enum<SelfTradePreventionCode>(block + 65);
}

inline void write_root_block(const ExecutionReportTrade& message, char* block) noexcept {
    little_endian::store64(block + 0, message.order_id);
    little_endian::store64(block + 8, message.cl_ord_id);
    little_endian::store64(block + 16, message.exec_id);
    little_endian::store64(block + 24, message.transact_time_ns);
    little_endian::store_signed64(block + 32, message.last_px_mantissa);
    little_endian::store64(block + 40, message.trd_match_id);
    little_endian::store32(block + 48, message.last_qty);
    little_endian::store32(block + 52, message.leaves_qty);
    little_endian::store32(block + 56, message.cum_qty);
    detail::store_symbol(block + 60, message.symbol);
    detail::store_enum(block + 68, message.side);
    detail::store_enum(block + 69, message.ord_status);
    detail::store_enum(block + 70, message.last_liquidity_ind);
}
inline void read_root_block(const char* block, ExecutionReportTrade& message) noexcept {
    message.order_id = little_endian::load64(block + 0);
    message.cl_ord_id = little_endian::load64(block + 8);
    message.exec_id = little_endian::load64(block + 16);
    message.transact_time_ns = little_endian::load64(block + 24);
    message.last_px_mantissa = little_endian::load_signed64(block + 32);
    message.trd_match_id = little_endian::load64(block + 40);
    message.last_qty = little_endian::load32(block + 48);
    message.leaves_qty = little_endian::load32(block + 52);
    message.cum_qty = little_endian::load32(block + 56);
    detail::load_symbol(block + 60, message.symbol);
    message.side = detail::load_enum<SideCode>(block + 68);
    message.ord_status = detail::load_enum<OrdStatusCode>(block + 69);
    message.last_liquidity_ind = detail::load_enum<LastLiquidityIndicator>(block + 70);
}

inline void write_root_block(const ExecutionReportCancel& message, char* block) noexcept {
    little_endian::store64(block + 0, message.order_id);
    little_endian::store64(block + 8, message.cl_ord_id);
    little_endian::store64(block + 16, message.orig_cl_ord_id);
    little_endian::store64(block + 24, message.exec_id);
    little_endian::store64(block + 32, message.transact_time_ns);
    little_endian::store32(block + 40, message.canceled_qty);
    little_endian::store32(block + 44, message.leaves_qty);
    little_endian::store32(block + 48, message.cum_qty);
    detail::store_symbol(block + 52, message.symbol);
    detail::store_enum(block + 60, message.side);
    detail::store_enum(block + 61, message.ord_status);
    detail::store_enum(block + 62, message.cancel_reason);
}
inline void read_root_block(const char* block, ExecutionReportCancel& message) noexcept {
    message.order_id = little_endian::load64(block + 0);
    message.cl_ord_id = little_endian::load64(block + 8);
    message.orig_cl_ord_id = little_endian::load64(block + 16);
    message.exec_id = little_endian::load64(block + 24);
    message.transact_time_ns = little_endian::load64(block + 32);
    message.canceled_qty = little_endian::load32(block + 40);
    message.leaves_qty = little_endian::load32(block + 44);
    message.cum_qty = little_endian::load32(block + 48);
    detail::load_symbol(block + 52, message.symbol);
    message.side = detail::load_enum<SideCode>(block + 60);
    message.ord_status = detail::load_enum<OrdStatusCode>(block + 61);
    message.cancel_reason = detail::load_enum<ExsimCancelReasonCode>(block + 62);
}

inline void write_root_block(const ExecutionReportReplace& message, char* block) noexcept {
    little_endian::store64(block + 0, message.order_id);
    little_endian::store64(block + 8, message.previous_order_id);
    little_endian::store64(block + 16, message.cl_ord_id);
    little_endian::store64(block + 24, message.orig_cl_ord_id);
    little_endian::store64(block + 32, message.exec_id);
    little_endian::store64(block + 40, message.transact_time_ns);
    little_endian::store_signed64(block + 48, message.price_mantissa);
    little_endian::store32(block + 56, message.order_qty);
    little_endian::store32(block + 60, message.leaves_qty);
    little_endian::store32(block + 64, message.cum_qty);
    detail::store_symbol(block + 68, message.symbol);
    detail::store_enum(block + 76, message.side);
    detail::store_enum(block + 77, message.ord_status);
    detail::store_enum(block + 78, message.priority_retained);
}
inline void read_root_block(const char* block, ExecutionReportReplace& message) noexcept {
    message.order_id = little_endian::load64(block + 0);
    message.previous_order_id = little_endian::load64(block + 8);
    message.cl_ord_id = little_endian::load64(block + 16);
    message.orig_cl_ord_id = little_endian::load64(block + 24);
    message.exec_id = little_endian::load64(block + 32);
    message.transact_time_ns = little_endian::load64(block + 40);
    message.price_mantissa = little_endian::load_signed64(block + 48);
    message.order_qty = little_endian::load32(block + 56);
    message.leaves_qty = little_endian::load32(block + 60);
    message.cum_qty = little_endian::load32(block + 64);
    detail::load_symbol(block + 68, message.symbol);
    message.side = detail::load_enum<SideCode>(block + 76);
    message.ord_status = detail::load_enum<OrdStatusCode>(block + 77);
    message.priority_retained = detail::load_enum<BooleanCode>(block + 78);
}

inline void write_root_block(const ExecutionReportReject& message, char* block) noexcept {
    little_endian::store64(block + 0, message.cl_ord_id);
    little_endian::store64(block + 8, message.exec_id);
    little_endian::store64(block + 16, message.transact_time_ns);
    detail::store_symbol(block + 24, message.symbol);
    detail::store_enum(block + 32, message.ord_rej_reason);
    detail::store_enum(block + 33, message.exsim_reject_reason);
}
inline void read_root_block(const char* block, ExecutionReportReject& message) noexcept {
    message.cl_ord_id = little_endian::load64(block + 0);
    message.exec_id = little_endian::load64(block + 8);
    message.transact_time_ns = little_endian::load64(block + 16);
    detail::load_symbol(block + 24, message.symbol);
    message.ord_rej_reason = detail::load_enum<OrdRejReasonCode>(block + 32);
    message.exsim_reject_reason = detail::load_enum<ExsimRejectReasonCode>(block + 33);
}

inline void write_root_block(const OrderCancelReject& message, char* block) noexcept {
    little_endian::store64(block + 0, message.cl_ord_id);
    little_endian::store64(block + 8, message.orig_cl_ord_id);
    little_endian::store64(block + 16, message.order_id);
    little_endian::store64(block + 24, message.transact_time_ns);
    detail::store_enum(block + 32, message.cxl_rej_response_to);
    detail::store_enum(block + 33, message.cxl_rej_reason);
    detail::store_enum(block + 34, message.exsim_reject_reason);
}
inline void read_root_block(const char* block, OrderCancelReject& message) noexcept {
    message.cl_ord_id = little_endian::load64(block + 0);
    message.orig_cl_ord_id = little_endian::load64(block + 8);
    message.order_id = little_endian::load64(block + 16);
    message.transact_time_ns = little_endian::load64(block + 24);
    message.cxl_rej_response_to = detail::load_enum<CxlRejResponseToCode>(block + 32);
    message.cxl_rej_reason = detail::load_enum<CxlRejReasonCode>(block + 33);
    message.exsim_reject_reason = detail::load_enum<ExsimRejectReasonCode>(block + 34);
}

inline void write_root_block(const OrderMassCancelReport& message, char* block) noexcept {
    little_endian::store64(block + 0, message.cl_ord_id);
    little_endian::store64(block + 8, message.transact_time_ns);
    little_endian::store32(block + 16, message.total_affected_orders);
    detail::store_symbol(block + 20, message.symbol);
    detail::store_enum(block + 28, message.mass_cancel_request_type);
    detail::store_enum(block + 29, message.mass_cancel_response);
    detail::store_enum(block + 30, message.mass_cancel_reject_reason);
    detail::store_enum(block + 31, message.exsim_reject_reason);
}
inline void read_root_block(const char* block, OrderMassCancelReport& message) noexcept {
    message.cl_ord_id = little_endian::load64(block + 0);
    message.transact_time_ns = little_endian::load64(block + 8);
    message.total_affected_orders = little_endian::load32(block + 16);
    detail::load_symbol(block + 20, message.symbol);
    message.mass_cancel_request_type = detail::load_enum<MassCancelRequestTypeCode>(block + 28);
    message.mass_cancel_response = detail::load_enum<MassCancelResponseCode>(block + 29);
    message.mass_cancel_reject_reason = detail::load_enum<MassCancelRejectReasonCode>(block + 30);
    message.exsim_reject_reason = detail::load_enum<ExsimRejectReasonCode>(block + 31);
}

inline void write_root_block(const BusinessMessageReject& message, char* block) noexcept {
    little_endian::store64(block + 0, message.ref_seq_num);
    little_endian::store16(block + 8, message.ref_msg_type);
    detail::store_enum(block + 10, message.business_reject_reason);
}
inline void read_root_block(const char* block, BusinessMessageReject& message) noexcept {
    message.ref_seq_num = little_endian::load64(block + 0);
    message.ref_msg_type = little_endian::load16(block + 8);
    message.business_reject_reason = detail::load_enum<BusinessRejectReasonCode>(block + 10);
}

// ---------------------------------------------------------------------------
// Framing.

struct MessageHeader {
    std::uint16_t block_length = 0;
    std::uint16_t template_id = 0;
    std::uint16_t schema_id = 0;
    std::uint16_t version = 0;
    std::uint16_t num_groups = 0;
    std::uint16_t num_var_data_fields = 0;
};

// One complete, header-validated frame inside the caller's buffer.
struct FrameView {
    const char* frame = nullptr;            // first SOFH byte
    std::size_t frame_size = 0;             // SOFH Message_Length
    MessageHeader header;
    const char* root_block = nullptr;       // header.block_length bytes
    const char* after_root_block = nullptr; // variable length data, then frame end
    std::size_t after_root_block_size = 0;
};

[[nodiscard]] inline DecodeStatus decode_frame(const char* bytes, std::size_t available, FrameView& view) noexcept {
    if (available < kSofhSize) return DecodeStatus::Incomplete;
    const std::uint32_t frame_size = wire::load32(bytes);
    if (wire::load16(bytes + 4) != kSofhEncodingTypeSbeLittleEndian) return DecodeStatus::UnsupportedEncodingType;
    if (frame_size < kFramePrefixSize) return DecodeStatus::FrameLengthTooSmall;
    if (frame_size > kMaxAcceptedFrameSize) return DecodeStatus::FrameLengthTooLarge;
    if (available < frame_size) return DecodeStatus::Incomplete;
    const char* header_bytes = bytes + kSofhSize;
    MessageHeader header;
    header.block_length = little_endian::load16(header_bytes + 0);
    header.template_id = little_endian::load16(header_bytes + 2);
    header.schema_id = little_endian::load16(header_bytes + 4);
    header.version = little_endian::load16(header_bytes + 6);
    header.num_groups = little_endian::load16(header_bytes + 8);
    header.num_var_data_fields = little_endian::load16(header_bytes + 10);
    if (header.schema_id != kSchemaId) return DecodeStatus::UnknownSchemaId;
    if (header.version < kSchemaVersion) return DecodeStatus::UnsupportedSchemaVersion;
    if (header.block_length > frame_size - kFramePrefixSize) return DecodeStatus::BlockExceedsFrame;
    view.frame = bytes;
    view.frame_size = frame_size;
    view.header = header;
    view.root_block = bytes + kFramePrefixSize;
    view.after_root_block = view.root_block + header.block_length;
    view.after_root_block_size = frame_size - kFramePrefixSize - header.block_length;
    return DecodeStatus::Ok;
}

template <typename Message>
[[nodiscard]] constexpr std::size_t encoded_frame_size(const Message& message) noexcept {
    std::size_t size = kFramePrefixSize + Message::kBlockLength;
    if constexpr (Message::kHasVariableLengthField) size += kVariableLengthPrefixSize + variable_length_field(message).size();
    return size;
}

// Writes one complete frame; returns its size, or 0 (nothing usable written)
// when the capacity is short or the variable length field is too long.
template <typename Message>
[[nodiscard]] std::size_t encode(const Message& message, char* out, std::size_t capacity) noexcept {
    if constexpr (Message::kHasVariableLengthField) {
        if (variable_length_field(message).size() > kMaxVariableLengthFieldBytes) return 0;
    }
    const std::size_t frame_size = encoded_frame_size(message);
    if (capacity < frame_size) return 0;
    wire::store32(out, static_cast<std::uint32_t>(frame_size));
    wire::store16(out + 4, kSofhEncodingTypeSbeLittleEndian);
    char* header_bytes = out + kSofhSize;
    little_endian::store16(header_bytes + 0, Message::kBlockLength);
    little_endian::store16(header_bytes + 2, template_id_value(Message::kTemplateId));
    little_endian::store16(header_bytes + 4, kSchemaId);
    little_endian::store16(header_bytes + 6, kSchemaVersion);
    little_endian::store16(header_bytes + 8, 0);
    little_endian::store16(header_bytes + 10, Message::kHasVariableLengthField ? 1 : 0);
    write_root_block(message, out + kFramePrefixSize);
    if constexpr (Message::kHasVariableLengthField) {
        const std::string_view field = variable_length_field(message);
        char* variable_bytes = out + kFramePrefixSize + Message::kBlockLength;
        little_endian::store16(variable_bytes, static_cast<std::uint16_t>(field.size()));
        for (std::size_t index = 0; index < field.size(); ++index) {
            variable_bytes[kVariableLengthPrefixSize + index] = field[index];
        }
    }
    return frame_size;
}

// Decodes the frame as Message. A header version above kSchemaVersion may
// carry a longer root block and extra trailing variable length fields; both
// are skipped (SBE 7.3.1, 7.4). Version 1 frames must match exactly.
template <typename Message>
[[nodiscard]] DecodeStatus decode(const FrameView& view, Message& message) noexcept {
    if (view.header.template_id != template_id_value(Message::kTemplateId)) return DecodeStatus::TemplateMismatch;
    if (view.header.block_length < Message::kBlockLength) return DecodeStatus::BlockLengthTooShort;
    if (view.header.num_groups != 0) return DecodeStatus::UnexpectedRepeatingGroups;
    const bool is_current_version = view.header.version == kSchemaVersion;
    std::size_t consumed_after_root_block = 0;
    if constexpr (Message::kHasVariableLengthField) {
        const bool count_matches = is_current_version ? view.header.num_var_data_fields == 1
                                                      : view.header.num_var_data_fields >= 1;
        if (!count_matches) return DecodeStatus::VariableLengthFieldCountMismatch;
        if (view.after_root_block_size < kVariableLengthPrefixSize) return DecodeStatus::VariableLengthFieldOverrun;
        const std::size_t field_size = little_endian::load16(view.after_root_block);
        if (field_size > kMaxVariableLengthFieldBytes) return DecodeStatus::VariableLengthFieldTooLong;
        if (view.after_root_block_size - kVariableLengthPrefixSize < field_size) {
            return DecodeStatus::VariableLengthFieldOverrun;
        }
        set_variable_length_field(message,
                                  std::string_view(view.after_root_block + kVariableLengthPrefixSize, field_size));
        consumed_after_root_block = kVariableLengthPrefixSize + field_size;
    } else {
        if (is_current_version && view.header.num_var_data_fields != 0) {
            return DecodeStatus::VariableLengthFieldCountMismatch;
        }
    }
    if (is_current_version && view.after_root_block_size != consumed_after_root_block) {
        return DecodeStatus::TrailingBytes;
    }
    read_root_block(view.root_block, message);
    return DecodeStatus::Ok;
}

}  // namespace exsim::protocol::sbe_order_entry

#endif
