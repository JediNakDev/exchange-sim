#include "exsim/api/postgres_store.hpp"
#include "exsim/api/sha256.hpp"

#include <mutex>
#include <stdexcept>
#include <chrono>
#include <thread>

#ifdef EXSIM_WITH_POSTGRES
#include <libpq-fe.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace exsim::api {
#ifdef EXSIM_WITH_POSTGRES
namespace {
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
const char* table(PostgresStore::Records kind) {
    switch (kind) {
    case PostgresStore::Records::Experiments: return "exsim.experiments";
    case PostgresStore::Records::Runs: return "exsim.runs";
    case PostgresStore::Records::Idempotency: return "exsim.idempotency";
    }
    throw std::logic_error("unknown record kind");
}
const char* key_column(PostgresStore::Records kind) {
    switch (kind) {
    case PostgresStore::Records::Experiments: return "experiment_id";
    case PostgresStore::Records::Runs: return "run_id";
    case PostgresStore::Records::Idempotency: return "key";
    }
    throw std::logic_error("unknown record kind");
}
}

struct PostgresStore::Implementation {
    std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, &PQfinish};
    std::mutex mutex;
    bool importing = false;
    int artifact_lock = -1;
    ~Implementation() { if (artifact_lock >= 0) ::close(artifact_lock); }

    Result execute(const std::string& sql, const std::vector<std::string>& parameters = {}) {
        std::vector<const char*> values;
        for (const auto& value : parameters) {
            if (value.find('\0') != std::string::npos) throw std::runtime_error("database parameter contains NUL");
            values.push_back(value.c_str());
        }
        Result result(parameters.empty() ? PQexec(connection.get(), sql.c_str()) :
            PQexecParams(connection.get(), sql.c_str(), static_cast<int>(values.size()), nullptr,
                         values.data(), nullptr, nullptr, 0), &PQclear);
        if (!result || (PQresultStatus(result.get()) != PGRES_COMMAND_OK && PQresultStatus(result.get()) != PGRES_TUPLES_OK)) {
            // Server details can contain record data or credentials. Expose only SQLSTATE.
            const char* code = result ? PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) : nullptr;
            throw std::runtime_error(std::string("PostgreSQL operation failed (SQLSTATE ") + (code ? code : "unavailable") + ")");
        }
        return result;
    }
};

PostgresStore::PostgresStore(const std::string& connection, const std::filesystem::path& artifact_root)
    : implementation_(std::make_unique<Implementation>()) {
    implementation_->artifact_lock = ::open((artifact_root / ".postgres-lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (implementation_->artifact_lock < 0)
        throw std::runtime_error("artifact directory lock is unavailable");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (::flock(implementation_->artifact_lock, LOCK_EX | LOCK_NB) != 0) {
        if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("another research API owns this artifact directory, or its lock is unavailable");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const char* keywords[] = {"dbname", "connect_timeout", "application_name", nullptr};
    const char* values[] = {connection.c_str(), "5", "exsim_api", nullptr};
    implementation_->connection.reset(PQconnectdbParams(keywords, values, 1));
    if (!implementation_->connection || PQstatus(implementation_->connection.get()) != CONNECTION_OK)
        throw std::runtime_error("cannot connect to PostgreSQL; check EXSIM_DATABASE_URL and service availability");
    implementation_->execute("SET search_path = pg_catalog; SET statement_timeout = '10s'; SET lock_timeout = '5s'; SET synchronous_commit = on; SET client_min_messages = warning;");
    auto locked = implementation_->execute("SELECT pg_try_advisory_lock(1163416393, 1)");
    if (std::string(PQgetvalue(locked.get(), 0, 0)) != "t")
        throw std::runtime_error("another research API owns this database; multiple API processes are not supported yet");
    implementation_->execute("BEGIN");
    static constexpr const char* schema =
#include "postgres_schema.inc"
    ;
    implementation_->execute(schema);
    const auto root = std::filesystem::canonical(artifact_root).string();
    implementation_->execute("INSERT INTO exsim.schema_version(singleton,version,artifact_root) VALUES(true,1,$1) ON CONFLICT DO NOTHING", {root});
    auto version = implementation_->execute("SELECT version,artifact_root FROM exsim.schema_version WHERE singleton");
    if (PQntuples(version.get()) != 1 || std::string(PQgetvalue(version.get(),0,0)) != "1")
        throw std::runtime_error("unsupported PostgreSQL metadata schema version");
    if (std::string(PQgetvalue(version.get(),0,1)) != root)
        throw std::runtime_error("database belongs to another artifact directory; use its original --data-dir");
    implementation_->execute("COMMIT");
}
PostgresStore::~PostgresStore() = default;
int PostgresStore::artifact_lock_fd() const { return implementation_->artifact_lock; }

void PostgresStore::put(Records kind, const std::string& record) {
    std::lock_guard lock(implementation_->mutex);
    std::string sql = "INSERT INTO " + std::string(table(kind)) + "(record) VALUES($1)";
    if (kind != Records::Experiments) {
        sql += " ON CONFLICT (" + std::string(key_column(kind)) + ") DO UPDATE SET record=EXCLUDED.record";
        const char* immutable = kind == Records::Runs ? "experiment_id" : "fingerprint";
        sql += " WHERE " + std::string(table(kind)) + "." + immutable + "=EXCLUDED." + immutable;
    }
    auto result = implementation_->execute(sql, {record});
    if (std::string(PQcmdTuples(result.get())) != "1") throw std::runtime_error("database record ownership conflict");
}
void PostgresStore::remove_idempotency(const std::string& key) {
    std::lock_guard lock(implementation_->mutex);
    implementation_->execute("DELETE FROM exsim.idempotency WHERE key=$1", {key});
}
std::vector<std::pair<std::filesystem::path, std::string>> PostgresStore::records(Records kind) {
    std::lock_guard lock(implementation_->mutex);
    auto result = implementation_->execute("SELECT " + std::string(key_column(kind)) + ",record FROM " + table(kind) + " ORDER BY 1");
    std::vector<std::pair<std::filesystem::path, std::string>> records;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        std::string key = PQgetvalue(result.get(), row, 0);
        if (kind == Records::Idempotency) key = sha256_hex(key);
        records.emplace_back(kind == Records::Runs ? key + "/run.json" : key + ".json", PQgetvalue(result.get(), row, 1));
    }
    return records;
}
bool PostgresStore::empty() {
    std::lock_guard lock(implementation_->mutex);
    auto result = implementation_->execute("SELECT NOT EXISTS(SELECT 1 FROM exsim.experiments) AND NOT EXISTS(SELECT 1 FROM exsim.runs) AND NOT EXISTS(SELECT 1 FROM exsim.idempotency)");
    return std::string(PQgetvalue(result.get(),0,0)) == "t";
}
std::string PostgresStore::run_record(const std::string& run_id) {
    std::lock_guard lock(implementation_->mutex);
    auto result = implementation_->execute("SELECT record FROM exsim.runs WHERE run_id=$1", {run_id});
    if (PQntuples(result.get()) != 1) throw std::runtime_error("authoritative run record is missing");
    return PQgetvalue(result.get(),0,0);
}
void PostgresStore::begin_transaction() {
    std::lock_guard lock(implementation_->mutex);
    if (implementation_->importing) throw std::logic_error("import already started");
    implementation_->execute("BEGIN");
    implementation_->importing = true;
}
void PostgresStore::commit_transaction() {
    std::lock_guard lock(implementation_->mutex);
    if (!implementation_->importing) throw std::logic_error("no import started");
    implementation_->execute("COMMIT");
    implementation_->importing = false;
}
void PostgresStore::rollback_transaction() {
    std::lock_guard lock(implementation_->mutex);
    implementation_->execute("ROLLBACK");
    implementation_->importing = false;
}
#else
struct PostgresStore::Implementation {};
PostgresStore::PostgresStore(const std::string&, const std::filesystem::path&) {
    throw std::runtime_error("PostgreSQL support requires make WITH_POSTGRES=1");
}
PostgresStore::~PostgresStore() = default;
int PostgresStore::artifact_lock_fd() const { return -1; }
void PostgresStore::put(Records, const std::string&) { throw std::logic_error("PostgreSQL unavailable"); }
void PostgresStore::remove_idempotency(const std::string&) { throw std::logic_error("PostgreSQL unavailable"); }
std::vector<std::pair<std::filesystem::path, std::string>> PostgresStore::records(Records) { throw std::logic_error("PostgreSQL unavailable"); }
bool PostgresStore::empty() { throw std::logic_error("PostgreSQL unavailable"); }
std::string PostgresStore::run_record(const std::string&) { throw std::logic_error("PostgreSQL unavailable"); }
void PostgresStore::begin_transaction() { throw std::logic_error("PostgreSQL unavailable"); }
void PostgresStore::commit_transaction() { throw std::logic_error("PostgreSQL unavailable"); }
void PostgresStore::rollback_transaction() { throw std::logic_error("PostgreSQL unavailable"); }
#endif
} // namespace exsim::api
