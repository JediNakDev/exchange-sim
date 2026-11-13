#ifndef EXSIM_GATEWAY_BYTE_SINK_HPP
#define EXSIM_GATEWAY_BYTE_SINK_HPP

// Output byte sink of every order-entry gateway (OUCH, FIX, iLink) and of the
// OUCH and FIX clients. The server adapts the net layer's send buffer to it;
// tests use an in-memory buffer.
//
//   char* prepare(std::size_t n) noexcept;  // n contiguous bytes, nullptr: no room
//   void  commit(std::size_t n) noexcept;   // n bytes written into the prepared span

#include <concepts>
#include <cstddef>

namespace exsim::gateway {

template <class Sink>
concept ByteSink = requires(Sink& sink, std::size_t byte_count) {
    { sink.prepare(byte_count) } noexcept -> std::same_as<char*>;
    { sink.commit(byte_count) } noexcept;
};

}  // namespace exsim::gateway

#endif
