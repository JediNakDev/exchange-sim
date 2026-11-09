#ifndef EXSIM_EXCHANGED_RUNTIME_HPP
#define EXSIM_EXCHANGED_RUNTIME_HPP
#include <string>
namespace exsim::api { int run_worker_process(int socket, const std::string& executable); }
#endif
