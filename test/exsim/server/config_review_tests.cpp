// Independent QA review reproducer (review of e2cdd11). Expected to FAIL until
// fixed. See /tmp/exsim-review-proto/REPORT.md, finding F3.

#include "md/publisher.hpp"
#include "server/config.hpp"
#include "../../framework/test.hpp"

#include <string>

using namespace exsim;
using namespace exsim::server;

namespace {
const char* kMinimalWithSmallPayload = R"(
[instrument AAPL]
tick_size = 0.01
min_price = 0.01
max_price = 1000
reference_price = 150.25

[user ALICE]
password = pw
session_id = 1

[market_data]
max_payload = 64
)";
}

// config.cpp accepts [market_data] max_payload in [64, 65507], but the ITCH
// publisher (and the retransmission server) require at least
// kMinimumMaxPayload = 20 + 2 + 50 = 72 bytes so a Net Order Imbalance message
// fits. A configuration that load_config validates must not make the
// publisher refuse to start.
TEST_CASE(server_config_review, accepted_max_payload_is_valid_for_the_publisher) {
    ServerConfig config;
    std::string error;
    const bool parsed = parse_config(kMinimalWithSmallPayload, config, error);
    if (!parsed) {
        // Rejecting 64 at load time is the fix; nothing more to check.
        CHECK(!error.empty());
        return;
    }
    md::PublisherConfig publisher_config;
    publisher_config.session = config.market_data.session;
    publisher_config.max_payload = config.market_data.max_payload;
    CHECK(md::validate_publisher_config(publisher_config) == md::PublishStatus::Ok);
}
