#include <server/journal/journal.hpp>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <system_error>
#include <sys/stat.h>
namespace exsim::server::journal {
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
    // Nonblocking open avoids waiting for a FIFO reader. Validate the
    // actual descriptor before truncating so special files are never used.
    fd_=::open(path.c_str(),O_WRONLY|O_CREAT|O_NONBLOCK|O_CLOEXEC,0600);
    if(fd_<0) throw std::system_error(errno,std::generic_category(),"journal open");
    try {
        struct stat info{};
        if(::fstat(fd_,&info)!=0) throw std::system_error(errno,std::generic_category(),"journal stat");
        if(!S_ISREG(info.st_mode)) throw std::invalid_argument("journal destination must be a regular file");
        if(::ftruncate(fd_,0)!=0 || ::lseek(fd_,0,SEEK_SET)<0)
            throw std::system_error(errno,std::generic_category(),"journal truncate");
        if(!write_all(fd_,header.data(),header.size())) throw std::system_error(errno,std::generic_category(),"journal header");
    }
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
}
