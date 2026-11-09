#ifndef LOB_ENGINE_THREAD_PINNING_HPP
#define LOB_ENGINE_THREAD_PINNING_HPP

#include <cstddef>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace lob::engine {

inline bool pin_current_thread_to_core(std::size_t core) noexcept {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<int>(core), &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy;
    policy.affinity_tag = static_cast<integer_t>(core + 1);
    return thread_policy_set(
        mach_thread_self(),
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
    (void)core;
    return false;
#endif
}

inline std::size_t hardware_threads() noexcept {
    std::size_t n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

}  // namespace lob::engine

#endif
