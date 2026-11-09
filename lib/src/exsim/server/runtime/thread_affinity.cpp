#include "exsim/server/runtime/thread_affinity.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>

#include <cstring>
#endif

namespace exsim::server::runtime {

bool pin_current_thread_to_cpu(int cpu, std::string& message) {
#if defined(__linux__)
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        message = "cpu " + std::to_string(cpu) + " is out of range";
        return false;
    }
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpu, &cpus);
    const int error = pthread_setaffinity_np(pthread_self(), sizeof cpus, &cpus);
    if (error != 0) {
        message = "pinning to cpu " + std::to_string(cpu) + " failed: " + std::strerror(error);
        return false;
    }
    message = "pinned to cpu " + std::to_string(cpu);
    return true;
#else
    message = "--cpu " + std::to_string(cpu) + " ignored: thread pinning is only supported on Linux";
    return false;
#endif
}

}  // namespace exsim::server::runtime
