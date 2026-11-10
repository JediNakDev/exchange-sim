#pragma once

#include <string>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#ifdef __APPLE__
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace bench {

class CPUPinner {
public:
    static bool pin(int core_id) {
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#elif defined(__APPLE__)
        thread_affinity_policy_data_t policy = {core_id};
        thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
        return thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY,
                                 reinterpret_cast<thread_policy_t>(&policy),
                                 THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
        (void)core_id;
        return false;
#endif
    }

    static std::string cpu_info() {
#ifdef __APPLE__
        return "Apple Silicon / macOS";
#elif defined(__linux__)
        return "Linux";
#else
        return "Unknown";
#endif
    }
};

}
