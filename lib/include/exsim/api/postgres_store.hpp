#ifndef EXSIM_API_POSTGRES_STORE_HPP
#define EXSIM_API_POSTGRES_STORE_HPP

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace exsim::api {

// Control-plane persistence only. The first migration retains versioned record
// encodings, with relational keys, constraints and indexes for run scheduling.
// One API owns the database. Local workers inherit its artifact-root fence,
// never its database connection.
class PostgresStore {
public:
    enum class Records { Experiments, Runs, Idempotency };
    PostgresStore(const std::string& connection, const std::filesystem::path& artifact_root);
    ~PostgresStore();
    PostgresStore(const PostgresStore&) = delete;
    PostgresStore& operator=(const PostgresStore&) = delete;

    void put(Records kind, const std::string& record);
    void remove_idempotency(const std::string& key);
    std::vector<std::pair<std::filesystem::path, std::string>> records(Records kind);
    bool empty();
    std::string run_record(const std::string& run_id);
    int artifact_lock_fd() const;
    // Used only during startup import. Validation and recovery happen before
    // commit; destruction rolls back if either fails.
    void begin_transaction();
    void commit_transaction();
    void rollback_transaction();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace exsim::api
#endif
