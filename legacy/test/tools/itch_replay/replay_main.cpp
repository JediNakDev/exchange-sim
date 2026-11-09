// itch_replay — replay a NASDAQ ITCH 5.0 binary file through the
// production OrderBook and report throughput + final book state.
//
// Usage:
//   itch_replay <file.itch>           # replay real ITCH file
//   itch_replay --synth N             # replay N synthetic messages
//   itch_replay --synth N --validate  # also diff against reference book
//
// File format assumption: raw concatenated ITCH 5.0 messages, no
// length-prefix framing. NASDAQ TotalView samples published as
// `.itch50` files use a 2-byte big-endian length prefix per message;
// pass --framed to strip it.

#include "diff.hpp"
#include "itch_replayer.hpp"
#include "reference_book.hpp"
#include "synthetic.hpp"

#include <lob/order_book.hpp>
#include <lob/protocol/itch.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { std::cerr << "cannot open " << path << "\n"; std::exit(1); }
    auto size = in.tellg();
    in.seekg(0);
    std::vector<char> buf(static_cast<std::size_t>(size));
    in.read(buf.data(), size);
    return buf;
}

// Strip 2-byte big-endian length prefix from each message in a NASDAQ
// `.itch50` framed dump. Returns a new buffer of concatenated raw bodies.
std::vector<char> unframe(const std::vector<char>& in) {
    std::vector<char> out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i + 2 <= in.size()) {
        std::uint16_t len = (static_cast<std::uint8_t>(in[i]) << 8)
                          |  static_cast<std::uint8_t>(in[i + 1]);
        i += 2;
        if (i + len > in.size()) break;
        out.insert(out.end(), in.begin() + i, in.begin() + i + len);
        i += len;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    bool synth = false;
    bool framed = false;
    bool validate = false;
    std::size_t synth_n = 0;
    std::string path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--synth" && i + 1 < argc) { synth = true; synth_n = std::stoull(argv[++i]); }
        else if (a == "--framed")           { framed = true; }
        else if (a == "--validate")         { validate = true; }
        else                                { path = a; }
    }
    if (!synth && path.empty()) {
        std::cerr << "usage: itch_replay <file.itch> [--framed] | --synth N [--validate]\n";
        return 1;
    }

    std::vector<char> wire;
    if (synth) {
        lob::tools::SyntheticItchStream gen(0xC0FFEE, synth_n, /*mid=*/10000);
        wire = gen.build();
    } else {
        auto raw = read_file(path);
        wire = framed ? unframe(raw) : std::move(raw);
    }

    lob::OrderBook book;
    lob::tools::ReferenceBook ref;
    lob::tools::ItchReplayer replayer(book, validate ? &ref : nullptr);

    std::size_t offset = 0;
    std::size_t parsed = 0;
    std::size_t skipped = 0;
    std::size_t mismatches = 0;

    auto t0 = std::chrono::steady_clock::now();
    while (offset < wire.size()) {
        int sz = lob::itch::message_size(wire[offset]);
        if (sz == 0) { ++skipped; ++offset; continue; }
        if (offset + static_cast<std::size_t>(sz) > wire.size()) break;
        lob::itch::Message m;
        if (lob::itch::parse(&wire[offset], m)) {
            replayer.apply(m);
            ++parsed;
            if (validate) {
                auto d = lob::tools::diff_books(book, ref, 10);
                if (!d.ok) {
                    if (++mismatches <= 3) {
                        std::cerr << "DIVERGENCE @msg " << parsed << ": " << d.detail << "\n";
                    }
                }
            }
        }
        offset += sz;
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    auto const& s = replayer.stats();
    std::cout << "file_bytes=" << wire.size()
              << " parsed=" << parsed
              << " skipped=" << skipped
              << " elapsed_s=" << secs
              << " msg_per_sec=" << (parsed / secs)
              << "\n"
              << "adds=" << s.adds
              << " executes=" << s.executes
              << " cancels=" << s.cancels
              << " deletes=" << s.deletes
              << " replaces=" << s.replaces
              << " unknown_ref=" << s.unknown_ref
              << " dropped_cross=" << s.dropped_cross
              << "\n";
    if (validate) std::cout << "validation_mismatches=" << mismatches << "\n";

    auto bid = book.get_best_bid();
    auto ask = book.get_best_ask();
    std::cout << "best_bid=" << (bid ? std::to_string(*bid) : "-")
              << " best_ask=" << (ask ? std::to_string(*ask) : "-")
              << " live_orders=" << book.get_total_orders() << "\n";
    return (validate && mismatches > 0) ? 2 : 0;
}
