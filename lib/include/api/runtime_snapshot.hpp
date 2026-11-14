#ifndef EXSIM_API_RUNTIME_SNAPSHOT_HPP
#define EXSIM_API_RUNTIME_SNAPSHOT_HPP
#include "server/server_runtime.hpp"
#include <string>
namespace exsim::api {
// The JSON object a managed exchange reports over its control socket and the
// API returns as an exchange's "runtime".
std::string runtime_snapshot_json(server::ServerRuntime& runtime);
}
#endif
