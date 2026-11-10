// End-to-end ITCH replay validation:
//   1. Generate a deterministic synthetic ITCH 5.0 stream.
//   2. Parse byte-by-byte through lob::itch::parse.
//   3. Drive both the production OrderBook and a ReferenceBook from
//      the same event sequence.
//   4. After every K messages diff the top-10 of both books — if they
//      ever disagree, fail.
//   5. After the full stream, assert ordering / live-order invariants.
//
// This is the harness that backs the "book reconstruction validated
// against a reference oracle" claim. Real NASDAQ ITCH files can be
// dropped through tools/itch_replay (same dispatch) with the same
// invariant checks toggled on.

#include "itch_replay_tests.hpp"
#include "test_framework.hpp"
#include "tools/itch_replay/diff.hpp"
#include "tools/itch_replay/itch_replayer.hpp"
#include "tools/itch_replay/reference_book.hpp"
#include "tools/itch_replay/synthetic.hpp"

#include <exchange/order_book.hpp>
#include "tools/itch_replay/itch.hpp"

#include <cassert>
#include <iostream>

namespace {

void replay_matches_reference() {
    constexpr std::size_t kMessages = 100'000;
    constexpr std::size_t kDiffEvery = 1;  // diff after every message

    lob::tools::SyntheticItchStream gen(/*seed=*/0xC0FFEE, kMessages, /*mid=*/10000);
    auto wire = gen.build();

    lob::OrderBook book;
    lob::tools::ReferenceBook ref;
    lob::tools::ItchReplayer replayer(book, &ref);

    std::size_t offset = 0;
    std::size_t parsed = 0;
    while (offset < wire.size()) {
        int sz = lob::itch::message_size(wire[offset]);
        assert(sz > 0 && "unknown message type in synthetic stream");
        assert(offset + static_cast<std::size_t>(sz) <= wire.size());

        lob::itch::Message m;
        bool ok = lob::itch::parse(&wire[offset], m);
        assert(ok);
        replayer.apply(m);
        offset += sz;
        ++parsed;

        if (parsed % kDiffEvery == 0) {
            auto d = lob::tools::diff_books(book, ref, /*depth=*/10);
            if (!d.ok) {
                std::cerr << "DIVERGENCE after " << parsed << " msgs (type='"
                          << static_cast<char>(m.type) << "'): "
                          << d.detail << "\n";
                std::exit(1);
            }
        }
    }

    auto final_diff = lob::tools::diff_books(book, ref, /*depth=*/20);
    if (!final_diff.ok) {
        std::cerr << "FINAL DIVERGENCE: " << final_diff.detail << "\n";
        std::exit(1);
    }

    auto const& s = replayer.stats();
    assert(s.total() == parsed && "every parsed message must be dispatched");
    assert(s.dropped_cross == 0 && "synthetic stream is non-marketable by construction");

    auto bid = book.get_best_bid();
    auto ask = book.get_best_ask();
    if (bid && ask) {
        assert(*bid < *ask && "engine reports crossed book");
    }

    std::cout << "[itch] parsed=" << parsed
              << " adds=" << s.adds
              << " execs=" << s.executes
              << " cancels=" << s.cancels
              << " deletes=" << s.deletes
              << " replaces=" << s.replaces
              << " unknown_ref=" << s.unknown_ref << " ";
}

}  // namespace

void itch_replay_tests() {
    std::cout << "\nITCH replay tests:\n";
    RUN_TEST(replay_matches_reference);
}
