#ifndef EXSIM_FUZZ_TARGETS_JOURNAL_READER_HPP
#define EXSIM_FUZZ_TARGETS_JOURNAL_READER_HPP

// Binary command journal (sim::JournalReader / sim::JournalWriter).
//
// The reader must reject any file that is not a well-formed journal with an
// exception derived from std::exception, never crash, and never expose a
// command before the whole file is validated. A file it accepts must be the
// canonical journal of its contents: writing the same setup and commands
// again with JournalWriter reproduces the file byte for byte.
//
// Input: the first byte picks the mode.
//   even: the rest of the input is the file, as is.
//   odd:  the rest is a journal without its integrity fields (header fields,
//         64-byte instruments, then 264-byte canonical command bodies); the
//         harness adds magic, schema, checksums, sequence numbers and the
//         footer, so mutations reach the semantic checks behind the hashes.

#include "fuzz_support.hpp"

#include <exsim/sim/journal.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace exsim::fuzz {

namespace journal_reader_fuzz {

inline constexpr std::uint64_t kHeaderMagic = 0x455853494d4a4e4cull;
inline constexpr std::uint64_t kFooterMagic = 0x455853494d454e44ull;
inline constexpr std::size_t kInstrumentBytes = 64;
inline constexpr std::size_t kCommandBodyBytes = 264;

inline void append_u64(std::vector<unsigned char>& bytes, std::uint64_t value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 8);
    sim::store64(bytes.data() + offset, value);
}

inline std::vector<unsigned char> build_journal_with_integrity_fields(std::span<const unsigned char> body) {
    std::vector<unsigned char> file;
    append_u64(file, kHeaderMagic);
    append_u64(file, sim::kSchema);
    std::size_t header_field_bytes = body.size() < 24 ? body.size() : 24;
    if (body.size() >= 24) {
        const std::uint64_t instrument_count = sim::load64(body.data() + 16);
        const std::uint64_t available_instruments = (body.size() - 24) / kInstrumentBytes;
        header_field_bytes += static_cast<std::size_t>(
                                  instrument_count < available_instruments ? instrument_count : available_instruments) *
                              kInstrumentBytes;
    }
    file.insert(file.end(), body.begin(), body.begin() + static_cast<std::ptrdiff_t>(header_field_bytes));
    append_u64(file, sim::hash_bytes(file));
    std::uint64_t record_count = 0;
    for (std::size_t offset = header_field_bytes; offset + kCommandBodyBytes <= body.size();
         offset += kCommandBodyBytes) {
        const std::size_t record_start = file.size();
        append_u64(file, record_count++);
        file.insert(file.end(), body.begin() + static_cast<std::ptrdiff_t>(offset),
                    body.begin() + static_cast<std::ptrdiff_t>(offset + kCommandBodyBytes));
        append_u64(file, sim::hash_bytes({file.data() + record_start, 272}));
    }
    const std::size_t footer_start = file.size();
    append_u64(file, kFooterMagic);
    append_u64(file, record_count);
    append_u64(file, sim::hash_bytes({file.data() + footer_start, 16}));
    return file;
}

// One scratch file per process and role; libFuzzer jobs are separate processes.
inline std::string scratch_path(std::string_view role) {
    return (std::filesystem::temp_directory_path() /
            ("exsim-fuzz-journal-" + std::to_string(::getpid()) + "-" + std::string(role) + ".bin"))
        .string();
}

inline void write_file(const std::string& path, std::span<const unsigned char> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    EXSIM_FUZZ_REQUIRE(out.good(), "the scratch journal can be written");
}

inline std::vector<unsigned char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

inline void check_journal_file(std::span<const unsigned char> file_bytes) {
    const std::string input_path = scratch_path("input");
    write_file(input_path, file_bytes);

    std::optional<sim::JournalReader> reader;
    try {
        reader.emplace(input_path);
    } catch (const std::exception&) {
        std::filesystem::remove(input_path);
        return;  // rejected: the documented outcome for anything malformed
    }
    std::filesystem::remove(input_path);

    const sim::JournalSetup& setup = reader->setup();
    EXSIM_FUZZ_REQUIRE(setup.instruments.size() <= setup.engine.max_instruments, "instrument count within the engine");
    for (std::size_t index = 0; index < setup.instruments.size(); ++index) {
        EXSIM_FUZZ_REQUIRE(valid_instrument_config(setup.instruments[index]), "accepted instruments are valid");
        for (std::size_t prior = 0; prior < index; ++prior) {
            EXSIM_FUZZ_REQUIRE(setup.instruments[prior].symbol != setup.instruments[index].symbol,
                               "accepted symbols are unique");
        }
    }
    std::vector<Command> commands;
    Command command;
    while (reader->next(command)) commands.push_back(command);
    EXSIM_FUZZ_REQUIRE(!reader->next(command), "next stays false at the end");

    const std::string rewritten_path = scratch_path("rewritten");
    bool rewritten = false;
    try {
        sim::JournalWriter writer(rewritten_path, setup);
        bool appended = true;
        for (const Command& each_command : commands) appended = writer.append(each_command) && appended;
        rewritten = appended && writer.flush();
    } catch (const std::exception&) {
        rewritten = false;
    }
    const std::vector<unsigned char> rewritten_bytes = read_file(rewritten_path);
    std::filesystem::remove(rewritten_path);
    EXSIM_FUZZ_REQUIRE(rewritten, "the writer accepts everything the reader accepted");
    EXSIM_FUZZ_REQUIRE(rewritten_bytes.size() == file_bytes.size() &&
                           std::equal(rewritten_bytes.begin(), rewritten_bytes.end(), file_bytes.begin()),
                       "an accepted journal is canonical: rewriting it reproduces every byte");
}

}  // namespace journal_reader_fuzz

inline int fuzz_journal_reader(const std::uint8_t* data, std::size_t size) {
    using namespace journal_reader_fuzz;
    if (size == 0) return 0;
    const std::span<const unsigned char> rest(data + 1, size - 1);
    if ((data[0] & 1) == 0) {
        check_journal_file(rest);
    } else {
        check_journal_file(build_journal_with_integrity_fields(rest));
    }
    return 0;
}

}  // namespace exsim::fuzz

#endif
