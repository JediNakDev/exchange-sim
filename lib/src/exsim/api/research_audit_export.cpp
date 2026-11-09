// Complete, fixed-name, bounded-memory native audit transfer. No user paths.
#include "exsim/api/research_run.hpp"
#include "exsim/api/postgres_store.hpp"
#include "exsim/api/api_json.hpp"
#include "exsim/api/sha256.hpp"
#include "exsim/research/run_artifacts.hpp"
#include <fstream>

namespace exsim::api {
namespace {
std::vector<std::string> artifact_names() {
    std::vector<std::string> names = {"manifest.json", "run.json", "summary.json", "events.bin", "fills.bin", "fill_positions.bin", "markouts.bin", "samples.bin"};
    for (const auto name : research::kRunArtifactFileNames) names.push_back("runner/" + std::string(name));
    names.push_back("runner/" + std::string(research::kExecutionTraceFileName));
    names.push_back("runner/provenance.json");
    return names;
}
}
std::string ResearchRun::render_audit_export_json(std::optional<std::uint64_t> artifact, std::uint64_t offset) const {
    std::lock_guard lock(mutex_);
    if (!is_terminal(state_)) throw ApiFailure(409, "run_not_terminal", "Audit export requires a terminal snapshot");
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(parameters_.run_directory))) throw ApiFailure(409, "artifact_unsafe", "Audit export refuses symbolic run directories");
    const auto names = artifact_names();
    // PostgreSQL owns run state. Ignore any stale run.json retained by import.
    const auto record = parameters_.metadata_store
        ? parameters_.metadata_store->run_record(parameters_.run_id) + "\n" : std::string{};
    const auto confined = [&](const std::string& name) {
        const auto path = parameters_.run_directory / name;
        for (auto current = path; current != parameters_.run_directory; current = current.parent_path()) {
            const auto status = std::filesystem::symlink_status(current);
            if (std::filesystem::is_symlink(status)) throw ApiFailure(409, "artifact_unsafe", "Audit export refuses symbolic links");
        }
        return path;
    };
    ApiJsonWriter json;
    json.begin_object().key("runId").string(parameters_.run_id);
    if (!artifact) {
        json.key("schemaVersion").string("exsim.audit-export/1");
        json.key("state").string(to_api_string(state_));
        json.key("reproductionSupported").boolean(state_ == ApiRunState::Completed);
        json.key("verificationCommand").string("python3 tools/audit/research_audit.py verify PACKAGE");
        json.key("reproductionCommand");
        if (state_ == ApiRunState::Completed) json.string("exsim_research verify PACKAGE/runner"); else json.null();
        json.key("items").begin_array();
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (parameters_.metadata_store && names[i] == "run.json") {
                json.begin_object().key("artifactIndex").unsigned_text(i).key("name").string(names[i])
                    .key("present").boolean(true).key("sizeBytes").unsigned_text(record.size())
                    .key("sha256").string(sha256_hex(record)).end_object();
                continue;
            }
            const auto path = confined(names[i]);
            const bool present = std::filesystem::exists(path);
            json.begin_object().key("artifactIndex").unsigned_text(i).key("name").string(names[i]);
            json.key("present").boolean(present);
            json.key("sizeBytes"); if (present) json.unsigned_text(std::filesystem::file_size(path)); else json.null();
            json.key("sha256");
            if (present) {
                if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > (std::uint64_t{2} << 30))
                    throw ApiFailure(413, "artifact_too_large", "Audit artifact exceeds the 2 GiB per-file export bound");
                json.string(sha256_file_hex(path));
            } else json.null();
            json.end_object();
        }
        json.end_array();
    } else {
        if (*artifact >= names.size()) throw ApiFailure(404, "not_found", "Unknown audit artifact index");
        if (parameters_.metadata_store && names[*artifact] == "run.json") {
            if (offset > record.size()) throw ApiFailure(400, "invalid_request", "Audit offset exceeds file size");
            const auto count = std::min<std::uint64_t>(65536, record.size() - offset);
            std::string hex;
            constexpr char digits[] = "0123456789abcdef";
            for (const unsigned char byte : std::string_view(record).substr(offset, count)) {
                hex += digits[byte >> 4]; hex += digits[byte & 15];
            }
            json.key("artifactIndex").unsigned_text(*artifact).key("offset").unsigned_text(offset).key("contentHex").string(hex);
            json.key("nextOffset"); if (offset + count < record.size()) json.unsigned_text(offset + count); else json.null();
            json.end_object();
            return json.take();
        }
        const auto path = confined(names[*artifact]);
        if (!std::filesystem::is_regular_file(path)) throw ApiFailure(404, "not_found", "Audit artifact is absent");
        const auto size = std::filesystem::file_size(path);
        if (size > (std::uint64_t{2} << 30)) throw ApiFailure(413, "artifact_too_large", "Audit artifact exceeds the 2 GiB per-file export bound");
        if (offset > size) throw ApiFailure(400, "invalid_request", "Audit offset exceeds file size");
        std::ifstream input(path, std::ios::binary);
        input.seekg(static_cast<std::streamoff>(offset));
        char bytes[65536];
        input.read(bytes, static_cast<std::streamsize>(std::min<std::uint64_t>(sizeof bytes, size - offset)));
        const auto count = static_cast<std::uint64_t>(input.gcount());
        if (count != std::min<std::uint64_t>(sizeof bytes, size - offset)) throw ApiFailure(409, "artifact_changed", "Audit artifact changed while reading");
        std::string hex; hex.reserve(count * 2);
        constexpr char digits[] = "0123456789abcdef";
        for (std::uint64_t i = 0; i < count; ++i) { const auto b = static_cast<unsigned char>(bytes[i]); hex += digits[b >> 4]; hex += digits[b & 15]; }
        json.key("artifactIndex").unsigned_text(*artifact).key("offset").unsigned_text(offset).key("contentHex").string(hex);
        json.key("nextOffset"); if (offset + count < size) json.unsigned_text(offset + count); else json.null();
    }
    json.end_object();
    return json.take();
}
} // namespace exsim::api
