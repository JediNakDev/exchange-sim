#ifndef EXSIM_SERVER_RUNTIME_THREAD_AFFINITY_HPP
#define EXSIM_SERVER_RUNTIME_THREAD_AFFINITY_HPP

#include <string>

namespace exsim::server::runtime {

// Pins the calling thread to one CPU (`--cpu N`). Linux only: elsewhere it
// does nothing and returns false with an explanation in `message`.
bool pin_current_thread_to_cpu(int cpu, std::string& message);

}  // namespace exsim::server::runtime

#endif
