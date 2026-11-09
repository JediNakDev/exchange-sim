#include "runtime.hpp"
#include <csignal>
#include <iostream>
#include <string_view>
int main(int argc, char** argv) {
    std::signal(SIGPIPE,SIG_IGN);
    if (argc == 3 && std::string_view(argv[1]) == "--strategy-fd" && std::string_view(argv[2]) == "3")
        return exsim::trader::run(3);
    std::cerr << "usage: exsim_traderd --strategy-fd 3 (launched for an exchange run)\n";
    return 2;
}
