#ifndef EXSIM_MD_RETRANSMISSION_SERVER_HPP
#define EXSIM_MD_RETRANSMISSION_SERVER_HPP

// MoldUDP64 retransmission (request) server. Answers a 20-byte request
// (session, first sequence, count) with ONE unicast MoldUDP64 packet holding
// the requested messages, byte-identical to their original transmission,
// taken from the publisher's RetransmissionRing.
//
// Policy:
//   * a request that is not exactly 20 bytes, or names another session, is
//     ignored (no response);
//   * count 0 or sequence 0 is ignored;
//   * count is capped at max_messages_per_response, and the response holds as
//     many consecutive messages as fit in max_payload; the client re-requests
//     the remainder;
//   * a range that extends past the newest published message is clipped to
//     it; a range starting after it is answered with nothing (not yet sent);
//   * a range whose FIRST sequence has already left the ring is answered with
//     nothing, even when later messages are still held: in-order delivery
//     cannot use a suffix, so the client must rebuild from a snapshot or give
//     up (MoldUdp64Receiver skips the gap after its retry limit).
//
// answer() never allocates; the response span is valid only during the call.

#include <md/feed_common.hpp>
#include <md/retransmission_ring.hpp>
#include <protocol/moldudp64.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

namespace exsim::md {

struct RetransmissionConfig {
    std::string_view session = "EXSIM";        // must equal the publisher's session
    std::size_t max_payload = 1400;            // bytes per response packet including header
    std::uint16_t max_messages_per_response = 1024;
};

enum class RetransmitStatus : std::uint8_t {
    Answered = 0,       // one response packet sent
    MalformedRequest,   // not a 20-byte request, sequence 0 or count 0
    WrongSession,
    OutsideRing,        // first requested message no longer held
    NotYetPublished,    // first requested message has not been sent yet
};

struct RetransmitResult {
    RetransmitStatus status = RetransmitStatus::MalformedRequest;
    std::uint64_t first_sequence = 0;  // of the response (Answered only)
    std::uint16_t message_count = 0;   // messages in the response (Answered only)
};

struct RetransmissionStats {
    std::uint64_t requests = 0;
    std::uint64_t responses = 0;
    std::uint64_t messages_resent = 0;
    std::uint64_t ignored = 0;  // every non-Answered request
};

class RetransmissionServer {
public:
    // Setup only: allocates the response buffer. The ring (normally
    // publisher.ring()) must outlive the server.
    RetransmissionServer(const RetransmissionRing& ring, const RetransmissionConfig& config)
        : ring_(ring),
          max_payload_(checked_payload(config.max_payload)),
          max_messages_per_response_(config.max_messages_per_response == 0 ? 1 : config.max_messages_per_response),
          response_storage_(std::make_unique<char[]>(max_payload_)) {
        if (!make_mold_session_name(config.session, session_name_))
            throw std::invalid_argument("MoldUDP64 session longer than 10 characters");
    }

    RetransmissionServer(const RetransmissionServer&) = delete;
    RetransmissionServer& operator=(const RetransmissionServer&) = delete;

    // `response_sink(std::span<const char> packet)` sends the unicast reply to
    // the requester (the server binds the requester endpoint into it).
    template <class ResponseSink>
    RetransmitResult answer(std::span<const char> request_datagram, ResponseSink&& response_sink) {
        ++stats_.requests;
        RetransmitResult result;
        protocol::moldudp64::Request request;
        if (protocol::moldudp64::decode(request_datagram.data(), request_datagram.size(), request) !=
                protocol::Status::ok ||
            request.sequence == 0 || request.count == 0) {
            return ignore(result, RetransmitStatus::MalformedRequest);
        }
        if (request.session != session_name_) return ignore(result, RetransmitStatus::WrongSession);
        const std::uint64_t newest = ring_.newest_sequence();
        if (newest == 0 || request.sequence > newest) return ignore(result, RetransmitStatus::NotYetPublished);
        if (request.sequence < ring_.oldest_sequence()) return ignore(result, RetransmitStatus::OutsideRing);

        const std::uint64_t wanted = request.count < max_messages_per_response_ ? request.count
                                                                                 : max_messages_per_response_;
        const std::uint64_t available = newest - request.sequence + 1;
        const std::uint64_t limit = wanted < available ? wanted : available;

        protocol::moldudp64::PacketBuilder builder(std::span<char>(response_storage_.get(), max_payload_),
                                                   max_payload_);
        (void)builder.reset(std::string_view(session_name_.data(), session_name_.size()), request.sequence);
        std::uint64_t packed = 0;
        while (packed < limit) {
            const std::span<const char> message = ring_.find(request.sequence + packed);
            if (message.empty() || builder.append(message) != protocol::Status::ok) break;
            ++packed;
        }
        if (packed == 0) return ignore(result, RetransmitStatus::OutsideRing);
        response_sink(builder.packet());
        ++stats_.responses;
        stats_.messages_resent += packed;
        result.status = RetransmitStatus::Answered;
        result.first_sequence = request.sequence;
        result.message_count = static_cast<std::uint16_t>(packed);
        return result;
    }

    [[nodiscard]] const RetransmissionStats& stats() const noexcept { return stats_; }

private:
    static std::size_t checked_payload(std::size_t max_payload) {
        if (max_payload < kMinimumMaxPayload || max_payload > kMaximumMaxPayload)
            throw std::invalid_argument("retransmission max_payload out of range");
        return max_payload;
    }

    RetransmitResult ignore(RetransmitResult result, RetransmitStatus status) noexcept {
        ++stats_.ignored;
        result.status = status;
        return result;
    }

    const RetransmissionRing& ring_;
    MoldSessionName session_name_{};
    std::size_t max_payload_;
    std::uint16_t max_messages_per_response_;
    std::unique_ptr<char[]> response_storage_;
    RetransmissionStats stats_{};
};

}  // namespace exsim::md

#endif
