#ifndef EXSIM_FUZZ_TARGETS_SBE_ORDER_ENTRY_HPP
#define EXSIM_FUZZ_TARGETS_SBE_ORDER_ENTRY_HPP

// SBE 1.0 order entry over SOFH framing (include/exsim/protocol/sbe_order_entry.hpp,
// schema docs/protocols/exsim_order_entry_sbe.xml).
//
// 1. Frame header: decode_frame on the input matches an independent model of
//    the SOFH and message header rules (length bounds, encoding type, schema,
//    version, block length), and an accepted view points inside the input.
// 2. Every one of the 25 message types is decoded from every accepted frame.
//    The status matches a model of the template, block length, group count,
//    variable length field and trailing byte rules. A decoded message
//    re-encodes within kMaxEncodedFrameSize; a canonical version 1 frame
//    re-encodes to exactly its input bytes, and any other accepted frame
//    (a later version with appended fields) normalizes to a canonical frame
//    that round-trips.
// 3. The input walked as a stream of back-to-back frames: each accepted frame
//    goes through the checks above, and the walk always makes progress.

#include "fuzz_support.hpp"

#include <exsim/protocol/sbe_order_entry.hpp>
#include <array>
#include <tuple>

namespace exsim::fuzz {

namespace sbe_order_entry_fuzz {

namespace sbe = protocol::sbe_order_entry;
using sbe::DecodeStatus;

using AllMessageTypes = std::tuple<
    sbe::Negotiate, sbe::NegotiationResponse, sbe::NegotiationReject, sbe::Establish, sbe::EstablishmentAck,
    sbe::EstablishmentReject, sbe::Sequence, sbe::UnsequencedHeartbeat, sbe::RetransmitRequest,
    sbe::Retransmission, sbe::RetransmitReject, sbe::Terminate, sbe::NotApplied, sbe::NewOrderSingle,
    sbe::OrderCancelRequest, sbe::OrderCancelReplaceRequest, sbe::OrderMassCancelRequest, sbe::ExecutionReportNew,
    sbe::ExecutionReportTrade, sbe::ExecutionReportCancel, sbe::ExecutionReportReplace, sbe::ExecutionReportReject,
    sbe::OrderCancelReject, sbe::OrderMassCancelReport, sbe::BusinessMessageReject>;

// Byte helpers written out here rather than taken from the codec, so the
// model does not share its arithmetic with the code under test.
inline std::size_t model_big_endian(std::span<const char> bytes, std::size_t offset, std::size_t width) noexcept {
    std::size_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + index]);
    }
    return value;
}

inline std::size_t model_little_endian16(std::span<const char> bytes, std::size_t offset) noexcept {
    return static_cast<unsigned char>(bytes[offset]) |
           (static_cast<std::size_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8);
}

// SOFH (big-endian Message_Length, Encoding_Type 0xEB50) then the 12-byte
// little-endian SBE header: blockLength, templateId, schemaId, version,
// numGroups, numVarDataFields.
struct ModelHeader {
    std::size_t frame_size = 0;
    std::size_t block_length = 0;
    std::size_t template_id = 0;
    std::size_t schema_id = 0;
    std::size_t version = 0;
    std::size_t num_groups = 0;
    std::size_t num_var_data_fields = 0;
};

inline DecodeStatus model_frame_status(std::span<const char> input, ModelHeader& header) noexcept {
    if (input.size() < 6) return DecodeStatus::Incomplete;
    header.frame_size = model_big_endian(input, 0, 4);
    if (model_big_endian(input, 4, 2) != 0xEB50) return DecodeStatus::UnsupportedEncodingType;
    if (header.frame_size < 6 + 12) return DecodeStatus::FrameLengthTooSmall;
    if (header.frame_size > 1024) return DecodeStatus::FrameLengthTooLarge;
    if (input.size() < header.frame_size) return DecodeStatus::Incomplete;
    header.block_length = model_little_endian16(input, 6);
    header.template_id = model_little_endian16(input, 8);
    header.schema_id = model_little_endian16(input, 10);
    header.version = model_little_endian16(input, 12);
    header.num_groups = model_little_endian16(input, 14);
    header.num_var_data_fields = model_little_endian16(input, 16);
    if (header.schema_id != 22597) return DecodeStatus::UnknownSchemaId;
    if (header.version < 1) return DecodeStatus::UnsupportedSchemaVersion;
    if (header.block_length > header.frame_size - 18) return DecodeStatus::BlockExceedsFrame;
    return DecodeStatus::Ok;
}

template <typename Message>
DecodeStatus model_message_status(std::span<const char> frame, const ModelHeader& header) noexcept {
    if (header.template_id != static_cast<std::size_t>(Message::kTemplateId)) return DecodeStatus::TemplateMismatch;
    if (header.block_length < Message::kBlockLength) return DecodeStatus::BlockLengthTooShort;
    if (header.num_groups != 0) return DecodeStatus::UnexpectedRepeatingGroups;
    const bool version_one = header.version == 1;
    const std::size_t after_root_block = header.frame_size - 18 - header.block_length;
    std::size_t consumed = 0;
    if constexpr (Message::kHasVariableLengthField) {
        if (version_one ? header.num_var_data_fields != 1 : header.num_var_data_fields < 1) {
            return DecodeStatus::VariableLengthFieldCountMismatch;
        }
        if (after_root_block < 2) return DecodeStatus::VariableLengthFieldOverrun;
        const std::size_t field_size = model_little_endian16(frame, 18 + header.block_length);
        if (field_size > 128) return DecodeStatus::VariableLengthFieldTooLong;
        if (after_root_block - 2 < field_size) return DecodeStatus::VariableLengthFieldOverrun;
        consumed = 2 + field_size;
    } else {
        if (version_one && header.num_var_data_fields != 0) return DecodeStatus::VariableLengthFieldCountMismatch;
    }
    if (version_one && after_root_block != consumed) return DecodeStatus::TrailingBytes;
    return DecodeStatus::Ok;
}

template <typename Message>
void check_message_type(std::span<const char> frame, const sbe::FrameView& view, const ModelHeader& header) {
    Message message{};
    const DecodeStatus status = sbe::decode(view, message);
    EXSIM_FUZZ_REQUIRE(status == model_message_status<Message>(frame, header),
                       "message decode status must match the template/block/var-data model");
    if (status != DecodeStatus::Ok) return;

    std::array<char, sbe::kMaxEncodedFrameSize + 16> encoded{};
    const std::size_t encoded_size = sbe::encode(message, encoded.data(), encoded.size());
    EXSIM_FUZZ_REQUIRE(encoded_size != 0, "a decoded message must re-encode");
    EXSIM_FUZZ_REQUIRE(encoded_size <= sbe::kMaxEncodedFrameSize,
                       "re-encoded frames fit the gateway's kMaxEncodedFrameSize buffers");
    EXSIM_FUZZ_REQUIRE(encoded_size == sbe::encoded_frame_size(message), "encode returns encoded_frame_size");
    EXSIM_FUZZ_REQUIRE(sbe::encode(message, encoded.data(), encoded_size - 1) == 0,
                       "encode refuses a buffer one byte short");

    const bool canonical = header.version == 1 && header.block_length == Message::kBlockLength;
    if (canonical) {
        EXSIM_FUZZ_REQUIRE(same_bytes({encoded.data(), encoded_size}, frame.first(header.frame_size)),
                           "a canonical version 1 frame re-encodes to its input bytes");
        return;
    }
    // A later version: the canonical version 1 form must itself round-trip.
    sbe::FrameView canonical_view;
    EXSIM_FUZZ_REQUIRE(sbe::decode_frame(encoded.data(), encoded_size, canonical_view) == DecodeStatus::Ok,
                       "the canonical frame passes the frame checks");
    Message canonical_message{};
    EXSIM_FUZZ_REQUIRE(sbe::decode(canonical_view, canonical_message) == DecodeStatus::Ok,
                       "the canonical frame decodes");
    std::array<char, sbe::kMaxEncodedFrameSize + 16> encoded_again{};
    const std::size_t encoded_again_size = sbe::encode(canonical_message, encoded_again.data(), encoded_again.size());
    EXSIM_FUZZ_REQUIRE(same_bytes({encoded_again.data(), encoded_again_size}, {encoded.data(), encoded_size}),
                       "normalizing a later-version frame is idempotent");
}

// Returns the frame size consumed, or 0 when no complete valid frame starts here.
inline std::size_t check_frame(std::span<const char> input) {
    ModelHeader header;
    const DecodeStatus model_status = model_frame_status(input, header);
    sbe::FrameView view;
    const DecodeStatus status = sbe::decode_frame(input.data(), input.size(), view);
    EXSIM_FUZZ_REQUIRE(status == model_status, "decode_frame status must match the SOFH/header model");
    if (status != DecodeStatus::Ok) return 0;
    EXSIM_FUZZ_REQUIRE(view.frame == input.data() && view.frame_size == header.frame_size, "view covers the frame");
    EXSIM_FUZZ_REQUIRE(view.root_block == input.data() + 18, "root block follows the 18-byte prefix");
    EXSIM_FUZZ_REQUIRE(view.after_root_block == view.root_block + header.block_length &&
                           view.after_root_block_size == header.frame_size - 18 - header.block_length,
                       "variable length data follows the root block and ends at the frame end");
    EXSIM_FUZZ_REQUIRE(view.header.template_id == header.template_id && view.header.version == header.version &&
                           view.header.num_var_data_fields == header.num_var_data_fields,
                       "header fields are read little-endian");
    std::apply([&](auto... message_types) { (check_message_type<decltype(message_types)>(input, view, header), ...); },
               AllMessageTypes{});
    return header.frame_size;
}

}  // namespace sbe_order_entry_fuzz

inline int fuzz_sbe_order_entry(const std::uint8_t* data, std::size_t size) {
    const std::span<const char> input = as_chars(data, size);
    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t consumed = sbe_order_entry_fuzz::check_frame(input.subspan(offset));
        if (consumed == 0) break;
        EXSIM_FUZZ_REQUIRE(consumed >= 18, "every accepted frame advances the stream");
        offset += consumed;
    }
    return 0;
}

}  // namespace exsim::fuzz

#endif
