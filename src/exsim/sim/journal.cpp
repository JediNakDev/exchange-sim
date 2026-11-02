#include <exsim/sim/journal.hpp>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iterator>
#include <system_error>
namespace exsim::sim {
namespace {
bool write_all(int fd,const unsigned char* p,std::size_t n) noexcept {
    while(n) { auto k=::write(fd,p,n);
        if(k<0 && errno==EINTR) continue;
        if(k<=0) return false;
        p+=k;n-=static_cast<std::size_t>(k);
    }
    return true;
}
void add(std::vector<unsigned char>& b,std::uint64_t v) {
    auto n=b.size();b.resize(n+8);store64(b.data()+n,v);
}
}
JournalWriter::JournalWriter(const std::string& path,const JournalSetup& setup) {
    (void)required_event_capacity(setup.engine);
    if(setup.instruments.size()>setup.engine.max_instruments) throw std::invalid_argument("instrument count");
    std::vector<unsigned char> header;
    add(header,0x455853494d4a4e4cull);add(header,kSchema);
    add(header,setup.engine.max_instruments);add(header,setup.engine.max_orders);
    add(header,setup.instruments.size());
    for(const auto& i:setup.instruments) {
        if(!valid_instrument_config(i)) throw std::invalid_argument("instrument config");
        for(char c:i.symbol.chars) header.push_back(static_cast<unsigned char>(c));
        add(header,static_cast<std::uint64_t>(i.tick_size));add(header,i.lot_size);add(header,i.max_order_qty);
        add(header,static_cast<std::uint64_t>(i.min_price));add(header,static_cast<std::uint64_t>(i.max_price));
        add(header,static_cast<std::uint64_t>(i.reference_price));add(header,static_cast<unsigned>(i.initial_phase));
    }
    add(header,hash_bytes(header));
    fd_=::open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd_<0) throw std::system_error(errno,std::generic_category(),"journal open");
    try { if(!write_all(fd_,header.data(),header.size())) throw std::system_error(errno,std::generic_category(),"journal header"); }
    catch(...) { ::close(fd_);fd_=-1;throw; }
}
JournalWriter::~JournalWriter() { if(fd_>=0) ::close(fd_); }
// Explicit flush required: destructor cannot report I/O failures.
bool JournalWriter::flush() noexcept {
    if(failed_) return false;
    if(footer_) return true;
    if(!write_all(fd_,buffer_.data(),used_)) {failed_=true;return false;}
    used_=0;
    unsigned char footer[24]{};
    store64(footer,0x455853494d454e44ull);store64(footer+8,sequence_);
    store64(footer+16,hash_bytes({footer,16}));
    if(!write_all(fd_,footer,24)) {failed_=true;return false;}
    footer_=true;return true;
}
bool JournalWriter::append(const Command& c) noexcept {
    if(failed_) return false;
    if(used_+280>buffer_.size() && !flush()) return false;
    if(footer_) {
        if(::lseek(fd_,-24,SEEK_CUR)<0) {failed_=true;return false;}
        footer_=false;
    }
    auto* p=buffer_.data()+used_;
    store64(p,sequence_);auto command=encode(c);
    std::copy(command.begin(),command.end(),p+8);
    store64(p+272,hash_bytes({p,272}));
    used_+=280;++sequence_;return true;
}
JournalReader::JournalReader(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("journal open");
    bytes_={std::istreambuf_iterator<char>(in),{}};
    auto get=[&](){if(bytes_.size()-offset_<8) throw std::runtime_error("truncated header");
        auto v=load64(bytes_.data()+offset_);offset_+=8;return v;};
    if(get()!=0x455853494d4a4e4cull || get()!=kSchema) throw std::runtime_error("journal magic/schema");
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
    if(hash!=hash_bytes({bytes_.data(),end})) throw std::runtime_error("config checksum");
    if(bytes_.size()-offset_<24) throw std::runtime_error("missing journal footer");
    auto tail=bytes_.size()-24;
    if(load64(bytes_.data()+tail)!=0x455853494d454e44ull ||
       load64(bytes_.data()+tail+16)!=hash_bytes({bytes_.data()+tail,16}) ||
       load64(bytes_.data()+tail+8)!=(tail-offset_)/280)
        throw std::runtime_error("journal footer/count");
    bytes_.resize(tail);
    // Validate the entire stream before exposing even its first command.
    if((bytes_.size()-offset_)%280) throw std::runtime_error("truncated record");
    for(auto p=offset_;p<bytes_.size();p+=280) {
        if(load64(bytes_.data()+p)!=(p-offset_)/280 ||
           load64(bytes_.data()+p+272)!=hash_bytes({bytes_.data()+p,272}))
            throw std::runtime_error("record sequence/checksum");
        auto c=decode_command(std::span<const unsigned char,264>(bytes_.data()+p+8,264));
        auto canonical=encode(c);
        if(!std::equal(canonical.begin(),canonical.end(),bytes_.data()+p+8))
            throw std::runtime_error("noncanonical record");
    }
}
bool JournalReader::next(Command& c) {
    if(offset_==bytes_.size()) return false;
    c=decode_command(std::span<const unsigned char,264>(bytes_.data()+offset_+8,264));
    offset_+=280;++sequence_;return true;
}
}
