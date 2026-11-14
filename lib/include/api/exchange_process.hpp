#ifndef EXSIM_API_EXCHANGE_PROCESS_HPP
#define EXSIM_API_EXCHANGE_PROCESS_HPP
#include "net/socket.hpp"
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
namespace exsim::api {
enum class ExchangeProcessState { Stopped, Running, Failed };
constexpr std::string_view to_string(ExchangeProcessState state) noexcept {
    switch (state) {
        case ExchangeProcessState::Stopped: return "stopped";
        case ExchangeProcessState::Running: return "running";
        case ExchangeProcessState::Failed: return "failed";
    }
    return "failed";
}
// One exsim_exchanged child and its private control socket. The owner
// serializes every call except has_exited(). A watcher thread blocks in
// waitid(WNOWAIT) and only flags the exit, so the pid stays reserved for
// kill() until the owner reaps it.
class ExchangeProcess {
public:
    ExchangeProcess() = default;
    ExchangeProcess(const ExchangeProcess&) = delete;
    ExchangeProcess& operator=(const ExchangeProcess&) = delete;
    ~ExchangeProcess();
    // False when posix_spawn fails; state() is then Failed with the reason.
    bool spawn(const std::filesystem::path& executable, const std::filesystem::path& config_path);
    // Waits up to 10 s for the child's first snapshot. On failure the child
    // is stopped and state() is Failed with the reason.
    bool await_readiness();
    // Asks the running child for a fresh snapshot with a connections object.
    // On failure the child is stopped and state() is Failed.
    bool read_status();
    // Reaps the child if it already exited; an unclean exit becomes Failed.
    void reap_if_exited();
    void request_stop() noexcept;
    // SIGTERM, then SIGKILL if the child has not exited within 4.5 s.
    void stop() noexcept;
    // Safe from any thread: a spawned child exited and is not reaped yet.
    [[nodiscard]] bool has_exited();
    void clear_failure() noexcept;
    [[nodiscard]] ExchangeProcessState state() const noexcept { return state_; }
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::string& snapshot() const noexcept { return snapshot_; }
private:
    bool reap() noexcept;
    void stop_after_failure(std::string message) noexcept;
    ExchangeProcessState state_ = ExchangeProcessState::Stopped;
    std::string error_, snapshot_;
    pid_t pid_ = -1;
    net::Socket control_;
    std::thread exit_watcher_;
    std::mutex exit_mutex_;
    std::condition_variable exit_condition_;
    bool exited_ = false;
};
}
#endif
