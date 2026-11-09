#ifndef EXSIM_SIM_JOURNAL_HPP
#define EXSIM_SIM_JOURNAL_HPP
#include <sim/canonical.hpp>
#include <instrument.hpp>
#include <string>
#include <vector>
namespace exsim::sim {
struct JournalSetup { EngineConfig engine; std::vector<InstrumentConfig> instruments; };
class JournalWriter {
public:
    JournalWriter(const std::string& path, const JournalSetup& setup);
    ~JournalWriter();
    JournalWriter(const JournalWriter&)=delete;
    JournalWriter& operator=(const JournalWriter&)=delete;
    [[nodiscard]] bool append(const Command& command) noexcept; // no allocation, including flush
    [[nodiscard]] bool flush() noexcept;
private:
    int fd_=-1;
    std::array<unsigned char,280*128> buffer_{};
    std::size_t used_=0;
    std::uint64_t sequence_=0;
    bool footer_=false;
    bool failed_=false;
};
class JournalReader {
public:
    explicit JournalReader(const std::string& path);
    const JournalSetup& setup() const { return setup_; }
    bool next(Command& command);
private:
    JournalSetup setup_;
    std::vector<unsigned char> bytes_;
    std::size_t offset_=0;
    std::uint64_t sequence_=0;
};
}
#endif
