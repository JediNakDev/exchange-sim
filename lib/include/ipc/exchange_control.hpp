#ifndef EXSIM_IPC_EXCHANGE_CONTROL_HPP
#define EXSIM_IPC_EXCHANGE_CONTROL_HPP
#include <string>
#include <string_view>
namespace exsim::ipc {
// Bounded JSON frames on a private local socket. Setup/operator path only.
void send_control(int fd, std::string_view message, int timeout_ms = 3000);
std::string receive_control(int fd, int timeout_ms = 3000);
}
#endif
