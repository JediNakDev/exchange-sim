#ifndef EXSIM_GATEWAY_OUCH_BYTE_SINK_HPP
#define EXSIM_GATEWAY_OUCH_BYTE_SINK_HPP

// Output byte sink used by the OUCH gateway (server contract section 2.4) and by
// the OUCH client transport. The server adapts the net layer's send buffer to
// it; tests use an in-memory buffer.
//
//   char* prepare(std::size_t n) noexcept;  // n contiguous bytes, nullptr: no room
//   void  commit(std::size_t n) noexcept;   // n bytes written into the prepared span

#include <concepts>
#include <cstddef>

namespace exsim::gateway::ouch {

template <class Sink>
concept OuchByteSink = requires(Sink& sink, std::size_t byte_count) {
    { sink.prepare(byte_count) } noexcept -> std::same_as<char*>;
    { sink.commit(byte_count) } noexcept;
};

}  // namespace exsim::gateway::ouch

#endif
