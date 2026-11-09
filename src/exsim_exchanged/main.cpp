#include "runtime.hpp"
#include "api/worker_process.hpp"
#include "ipc/strategy_wire.hpp"
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--run-worker-fd" && std::string_view(argv[2]) == "3")
        return exsim::api::run_worker_process(3,exsim::ipc::daemon_path("exsim_exchanged"));
    std::cerr << "usage: exsim_exchanged --run-worker-fd 3 (launched by exsim_apid)\n";
    return 2;
}
