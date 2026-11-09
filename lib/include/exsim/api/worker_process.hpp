#ifndef EXSIM_API_WORKER_PROCESS_HPP
#define EXSIM_API_WORKER_PROCESS_HPP

#include "exsim/api/http_message.hpp"
#include <cstdint>
#include <mutex>
#include <string>

namespace exsim::api {

class WorkerRootLock {
public:
    explicit WorkerRootLock(const std::string& path);
    ~WorkerRootLock();
    int descriptor() const { return descriptor_; }
    WorkerRootLock(const WorkerRootLock&) = delete;
    WorkerRootLock& operator=(const WorkerRootLock&) = delete;
private:
    int descriptor_ = -1;
};

struct WorkerReply {
    HttpResponse response;
    std::string record;
    std::string run_view;
};
void require_worker_success(const WorkerReply& reply);

// Local control plane only. A worker advances independently of all RPCs.
// Its inherited socket is both its private command channel and lifetime fence.
class WorkerProcess {
public:
    WorkerProcess(const std::string& executable, const std::string& bootstrap, int ownership_fd);
    ~WorkerProcess();
    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;
    WorkerReply request(const HttpRequest& request);
    WorkerReply shutdown();
    void stop() noexcept;
private:
    WorkerReply exchange(const std::string& message);
    int socket_ = -1;
    int process_ = -1;
    std::mutex mutex_;
};

// Framed messages are bounded. Caller supplies a nonblocking socket; header
// and payload transfers each have a finite deadline, except the idle wait.
std::string read_worker_frame(int socket, bool idle = false);
void write_worker_frame(int socket, const std::string& message);

} // namespace exsim::api
#endif
