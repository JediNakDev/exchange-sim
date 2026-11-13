#ifndef EXSIM_TEST_SUPPORT_JOURNAL_READER_HPP
#define EXSIM_TEST_SUPPORT_JOURNAL_READER_HPP

// Reads back a journal written by server::journal::JournalWriter and checks
// its header, record checksums, sequence numbers and footer before returning
// the first command. Tests use it to replay journals.

#include <server/journal/canonical.hpp>
#include <server/journal/journal.hpp>

#include <algorithm>
#include <bit>
#include <climits>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace exsim::testsupport {

class JournalReader {
public:
    explicit JournalReader(const std::string& path);
    const server::journal::JournalSetup& setup() const { return setup_; }
    bool next(Command& command);
private:
    server::journal::JournalSetup setup_;
    std::vector<unsigned char> bytes_;
    std::size_t offset_=0;
    std::uint64_t sequence_=0;
};

inline JournalReader::JournalReader(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("journal open");
    bytes_={std::istreambuf_iterator<char>(in),{}};
    auto get=[&](){if(bytes_.size()-offset_<8) throw std::runtime_error("truncated header");
        auto v=server::journal::load64(bytes_.data()+offset_);offset_+=8;return v;};
    if(get()!=0x455853494d4a4e4cull || get()!=server::journal::kSchema) throw std::runtime_error("journal magic/schema");
    setup_.engine.max_instruments=get();setup_.engine.max_orders=get();
    (void)required_event_capacity(setup_.engine);
    auto count=get();
    if(count>setup_.engine.max_instruments || count>(bytes_.size()-offset_)/64)
        throw std::runtime_error("instrument count");
    for(std::uint64_t n=0;n<count;++n) {
        InstrumentConfig i;
        for(auto& c:i.symbol.chars) c=static_cast<char>(bytes_[offset_++]);
        i.tick_size=std::bit_cast<Price>(get());
        auto lot=get(),qty=get();
        if(lot>UINT32_MAX || qty>UINT32_MAX) throw std::runtime_error("quantity width");
        i.lot_size=static_cast<Quantity>(lot);i.max_order_qty=static_cast<Quantity>(qty);
        i.min_price=std::bit_cast<Price>(get());i.max_price=std::bit_cast<Price>(get());
        i.reference_price=std::bit_cast<Price>(get());auto phase=get();
        if(phase>4) throw std::runtime_error("phase");
        i.initial_phase=static_cast<Phase>(phase);
        if(!valid_instrument_config(i)) throw std::runtime_error("instrument config");
        for(const auto& prior:setup_.instruments) if(prior.symbol==i.symbol) throw std::runtime_error("duplicate symbol");
        setup_.instruments.push_back(i);
    }
    auto end=offset_;auto hash=get();
    if(hash!=server::journal::hash_bytes({bytes_.data(),end})) throw std::runtime_error("config checksum");
    if(bytes_.size()-offset_<24) throw std::runtime_error("missing journal footer");
    auto tail=bytes_.size()-24;
    if(server::journal::load64(bytes_.data()+tail)!=0x455853494d454e44ull ||
       server::journal::load64(bytes_.data()+tail+16)!=server::journal::hash_bytes({bytes_.data()+tail,16}) ||
       server::journal::load64(bytes_.data()+tail+8)!=(tail-offset_)/280)
        throw std::runtime_error("journal footer/count");
    bytes_.resize(tail);
    // Validate the entire stream before exposing even its first command.
    if((bytes_.size()-offset_)%280) throw std::runtime_error("truncated record");
    for(auto p=offset_;p<bytes_.size();p+=280) {
        if(server::journal::load64(bytes_.data()+p)!=(p-offset_)/280 ||
           server::journal::load64(bytes_.data()+p+272)!=server::journal::hash_bytes({bytes_.data()+p,272}))
            throw std::runtime_error("record sequence/checksum");
        auto c=server::journal::decode_command(std::span<const unsigned char,264>(bytes_.data()+p+8,264));
        auto canonical=server::journal::encode(c);
        if(!std::equal(canonical.begin(),canonical.end(),bytes_.data()+p+8))
            throw std::runtime_error("noncanonical record");
    }
}
inline bool JournalReader::next(Command& c) {
    if(offset_==bytes_.size()) return false;
    c=server::journal::decode_command(std::span<const unsigned char,264>(bytes_.data()+offset_+8,264));
    offset_+=280;++sequence_;return true;
}

}  // namespace exsim::testsupport

#endif
