#include "exsim/api/worker_process.hpp"
#include "test/framework/test.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <thread>

namespace {
struct SocketPair {
    int descriptors[2]{-1, -1};
    SocketPair() { if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) throw std::runtime_error("socketpair failed"); }
    ~SocketPair() { for (const int descriptor : descriptors) if (descriptor >= 0) ::close(descriptor); }
};
}

TEST_CASE(api_worker_protocol, roundtrip_spans_multiple_socket_reads) {
    SocketPair pair;
    std::string bytes;
    for (std::size_t index = 0; index < 200000; ++index) bytes += static_cast<char>(index % 256);
    std::thread writer([&] { exsim::api::write_worker_frame(pair.descriptors[0], bytes); });
    const auto received = exsim::api::read_worker_frame(pair.descriptors[1]);
    writer.join();
    CHECK_EQ(received, bytes);
}

TEST_CASE(api_worker_protocol, refuses_zero_and_oversized_headers_before_payload_allocation) {
    for (const unsigned char size : {0u, 255u}) {
        SocketPair pair;
        const char header[4]{static_cast<char>(size), 0, 0, 0};
        REQUIRE_EQ(::write(pair.descriptors[0], header, sizeof header), 4);
        bool rejected = false;
        try { (void)exsim::api::read_worker_frame(pair.descriptors[1]); }
        catch (const std::exception&) { rejected = true; }
        CHECK(rejected);
    }
}

TEST_CASE(api_worker_protocol, truncated_payload_and_closed_peer_fail_without_sigpipe) {
    SocketPair pair;
    const char header[4]{0, 0, 0, 10};
    REQUIRE_EQ(::write(pair.descriptors[0], header, sizeof header), 4);
    ::close(pair.descriptors[0]); pair.descriptors[0] = -1;
    bool read_rejected = false;
    try { (void)exsim::api::read_worker_frame(pair.descriptors[1]); }
    catch (const std::exception&) { read_rejected = true; }
    CHECK(read_rejected);
#ifdef __linux__
    // MSG_NOSIGNAL is per-send, including callers outside the API executable.
    bool write_rejected = false;
    try { exsim::api::write_worker_frame(pair.descriptors[1], "closed"); }
    catch (const std::exception&) { write_rejected = true; }
    CHECK(write_rejected);
#endif
}
