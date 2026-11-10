#ifndef EXSIM_MD_MDP_REPLAY_SESSION_HPP
#define EXSIM_MD_MDP_REPLAY_SESSION_HPP

#include "md/mdp_publisher.hpp"
#include "protocol/fix44.hpp"

namespace exsim::md {

// CME Global TCP Recovery: ASCII requests, SBE schema-9 responses. Each
// response is a uint16 LE packet length followed by the original UDP packet.
// A connection accepts one recovery request, then logs out.
template<class Sink>
class MdpReplaySession {
public:
    MdpReplaySession(const server::MarketDataSection& config, const MdpReplayRing& ring)
        : config_(config), ring_(ring) {}
    void open(Sink sink, std::uint64_t now) noexcept {
        sink_ = sink;
        phase_ = Phase::Login;
        close_ = false;
        overflow_ = false;
        deadline_ = now + kTimeout;
        admin_sequence_ = 1;
    }
    std::size_t on_data(std::span<const char> bytes, std::uint64_t now, std::uint64_t epoch) noexcept {
        namespace fix = protocol::fix44;
        std::size_t consumed = 0;
        for (unsigned frames = 0; !bytes.empty() && !close_ && frames < 64; ++frames) {
            std::size_t size = 0;
            const bool compact = bytes.front() == '9';
            auto status = protocol::Status::incomplete;
            if (compact) {
                const auto text = std::string_view(bytes.data(), bytes.size());
                const auto end = text.find(fix::soh);
                if (end != std::string_view::npos) {
                    std::uint64_t body = 0;
                    if (end > 16 || text.size() < 3 || text[1] != '=' ||
                        !fix::parse_uint(text.substr(2, end - 2), body) || body > 4096) status = protocol::Status::invalid_length;
                    else {
                        size = end + 1 + static_cast<std::size_t>(body) + 7;
                        if (size <= bytes.size()) status = protocol::Status::ok;
                    }
                } else if (bytes.size() > 16) status = protocol::Status::invalid_length;
            } else status = fix::frame_size(bytes, size);
            if (status == protocol::Status::incomplete) break;
            if (status != protocol::Status::ok || size > 4096) { logout("invalid frame", now, epoch); return consumed + bytes.size(); }
            auto frame = bytes.first(size);
            std::array<char, 4106> normalized{};
            if (compact) {
                std::uint64_t checksum = 0;
                const auto text = std::string_view(frame.data(), frame.size());
                unsigned sum = 0;
                for (std::size_t i = 0; i < size - 7; ++i) sum += static_cast<unsigned char>(frame[i]);
                if (text.substr(size - 7, 3) != "10=" || text.back() != fix::soh ||
                    !fix::parse_uint(text.substr(size - 4, 3), checksum) || checksum != sum % 256) {
                    logout("invalid checksum", now, epoch); return consumed + size;
                }
                std::memcpy(normalized.data(), "8=FIX.4.4\1", 10);
                std::memcpy(normalized.data() + 10, frame.data(), size);
                sum = 0;
                for (std::size_t i = 0; i < size + 3; ++i) sum += static_cast<unsigned char>(normalized[i]);
                sum %= 256;
                normalized[size + 6] = static_cast<char>('0' + sum / 100);
                normalized[size + 7] = static_cast<char>('0' + sum / 10 % 10);
                normalized[size + 8] = static_cast<char>('0' + sum % 10);
                frame = std::span<const char>(normalized.data(), size + 10);
            }
            fix::Parser<> parser;
            if (parser.parse(frame) != protocol::Status::ok) { logout("invalid request", now, epoch); return consumed + size; }
            const auto type = parser.get(35);
            if (type == "5") { close_ = true; return consumed + size; }
            if (phase_ == Phase::Login) {
                if (type != "A" || parser.get(553) != config_.replay_username || parser.get(554) != config_.replay_password)
                    logout("authentication failed", now, epoch);
                else {
                    const std::array<char, 1> body{30};
                    send_admin(408, body, epoch);
                    phase_ = Phase::Request;
                    deadline_ = now + kTimeout;
                }
            } else if (phase_ == Phase::Request) {
                std::uint64_t channel = 0, begin = 0, end = 0;
                const auto request_id = parser.get(262);
                const bool valid_request_id = !request_id.empty() && request_id.size() <= 32 &&
                    std::all_of(request_id.begin(), request_id.end(), [](char ch) { return ch >= 0x21 && ch <= 0x7e; });
                if (type != "V" || !valid_request_id || !parser.get_uint(1180, channel) || channel != config_.channel_id ||
                    !parser.get_uint(1182, begin) || !parser.get_uint(1183, end) || begin == 0 ||
                    end < begin || end >= UINT32_MAX || end - begin >= 2000)
                    logout("invalid recovery range or channel", now, epoch);
                else replay(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end), now, epoch);
            }
            // Once the single request is answered, ignore further requests
            // while waiting for the client's Logout or the five-second limit.
            consumed += size;
            bytes = bytes.subspan(size);
        }
        return consumed;
    }
    void on_timer(std::uint64_t now, std::uint64_t epoch) noexcept {
        if (close_ || now < deadline_) return;
        if (phase_ == Phase::Logout) close_ = true;
        else logout("request timeout", now, epoch);
    }
    void shutdown(std::uint64_t now, std::uint64_t epoch) noexcept { logout("exchange stopped", now, epoch); close_ = true; }
    [[nodiscard]] bool wants_close() const noexcept { return close_; }
    [[nodiscard]] bool flush_before_close() const noexcept { return !overflow_; }
private:
    enum class Phase { Login, Request, Logout };
    static constexpr std::uint64_t kTimeout = 5000000000;
    void send_admin(std::uint16_t id, std::span<const char> body, std::uint64_t epoch) noexcept {
        std::array<char, 190> message{};
        std::array<char, 202> packet{};
        const auto size = protocol::cme::encode_mdp_message({static_cast<std::uint16_t>(body.size()), id, 9, 0}, body, message);
        const auto length = protocol::cme::encode_mdp_packet(admin_sequence_++, epoch, std::span(message).first(size), packet);
        char* out = sink_.prepare(length + 2);
        if (!out || !length) { overflow_ = close_ = true; return; }
        protocol::cme::store_le(out, static_cast<std::uint16_t>(length));
        std::memcpy(out + 2, packet.data(), length);
        sink_.commit(length + 2);
    }
    void logout(std::string_view reason, std::uint64_t now, std::uint64_t epoch) noexcept {
        if (phase_ == Phase::Logout || close_) return;
        std::array<char, 180> body{};
        std::memcpy(body.data(), reason.data(), std::min(reason.size(), body.size()));
        send_admin(409, body, epoch);
        phase_ = Phase::Logout;
        deadline_ = now + kTimeout;
    }
    void replay(std::uint32_t begin, std::uint32_t end, std::uint64_t now, std::uint64_t epoch) noexcept {
        std::size_t total = 0;
        for (std::uint32_t sequence = begin; sequence <= end; ++sequence) {
            const auto packet = ring_.get(sequence);
            if (packet.empty()) { logout("requested history unavailable", now, epoch); return; }
            total += 2 + packet.size();
        }
        char* out = sink_.prepare(total);
        if (!out) { logout("replay exceeds connection capacity", now, epoch); return; }
        std::size_t offset = 0;
        for (std::uint32_t sequence = begin; sequence <= end; ++sequence) {
            const auto packet = ring_.get(sequence);
            protocol::cme::store_le(out + offset, static_cast<std::uint16_t>(packet.size()));
            std::memcpy(out + offset + 2, packet.data(), packet.size());
            offset += packet.size() + 2;
        }
        sink_.commit(total);
        logout("replay complete", now, epoch);
    }
    const server::MarketDataSection& config_;
    const MdpReplayRing& ring_;
    Sink sink_{};
    Phase phase_ = Phase::Login;
    std::uint64_t deadline_ = 0;
    std::uint32_t admin_sequence_ = 1;
    bool close_ = false, overflow_ = false;
};
}  // namespace exsim::md
#endif
