#ifndef EXSIM_SIM_CANONICAL_HPP
#define EXSIM_SIM_CANONICAL_HPP
#include <sim/fields.hpp>
#include <array>
#include <bit>
#include <stdexcept>
#include <span>
namespace exsim::sim {
// Schema 1: tag followed by active fields, each an unsigned 64-bit big-endian
// slot; signed prices use their two's-complement bit pattern.
inline constexpr std::uint64_t kSchema = 1;
inline void store64(unsigned char* p, std::uint64_t v) noexcept {
    for (unsigned i=0;i<8;++i) p[i]=static_cast<unsigned char>(v >> (56-8*i));
}
inline std::uint64_t load64(const unsigned char* p) noexcept {
    std::uint64_t v=0; for(unsigned i=0;i<8;++i) v=(v<<8)|p[i]; return v;
}
inline std::uint64_t hash_bytes(std::span<const unsigned char> bytes,
                               std::uint64_t h=14695981039346656037ull) noexcept {
    for(auto b:bytes) { h ^= b; h *= 1099511628211ull; } return h;
}
template<class T> inline auto encode(const T& value) {
    std::array<unsigned char,264> bytes{};
    store64(bytes.data(),static_cast<std::uint64_t>(value.type));
    std::size_t offset=8;
    visit_fields(value,[&](const Field& f){store64(bytes.data()+offset,f.raw);offset+=8;});
    return bytes;
}
inline std::uint64_t hash_event(const Event& e, std::uint64_t h=14695981039346656037ull) {
    return hash_bytes(encode(e),h);
}
inline Command decode_command(std::span<const unsigned char,264> bytes) {
    std::size_t offset=8;
    auto get=[&](){auto v=load64(bytes.data()+offset);offset+=8;return v;};
    Command c;
    switch(static_cast<CommandType>(load64(bytes.data()))) {
case CommandType::NewOrder: { NewOrder x;
x.ts = static_cast<decltype(x.ts)>(get());
x.instrument = static_cast<decltype(x.instrument)>(get());
x.session = static_cast<decltype(x.session)>(get());
x.owner = static_cast<decltype(x.owner)>(get());
x.cl_ord_id = static_cast<decltype(x.cl_ord_id)>(get());
x.side = static_cast<decltype(x.side)>(get());
x.type = static_cast<decltype(x.type)>(get());
x.tif = static_cast<decltype(x.tif)>(get());
x.stp = static_cast<decltype(x.stp)>(get());
x.post_only = static_cast<decltype(x.post_only)>(get());
x.price = std::bit_cast<Price>(get());
x.qty = static_cast<decltype(x.qty)>(get());
return Command{x}; }
case CommandType::Cancel: { CancelOrder x;
x.ts = static_cast<decltype(x.ts)>(get());
x.session = static_cast<decltype(x.session)>(get());
x.orig_cl_ord_id = static_cast<decltype(x.orig_cl_ord_id)>(get());
x.cl_ord_id = static_cast<decltype(x.cl_ord_id)>(get());
x.reduce_to_total_qty = static_cast<decltype(x.reduce_to_total_qty)>(get());
return Command{x}; }
case CommandType::Replace: { ReplaceOrder x;
x.ts = static_cast<decltype(x.ts)>(get());
x.session = static_cast<decltype(x.session)>(get());
x.orig_cl_ord_id = static_cast<decltype(x.orig_cl_ord_id)>(get());
x.cl_ord_id = static_cast<decltype(x.cl_ord_id)>(get());
x.price = std::bit_cast<Price>(get());
x.qty = static_cast<decltype(x.qty)>(get());
x.force_priority_loss = static_cast<decltype(x.force_priority_loss)>(get());
return Command{x}; }
case CommandType::MassCancel: { MassCancel x;
x.ts = static_cast<decltype(x.ts)>(get());
x.session = static_cast<decltype(x.session)>(get());
x.instrument = static_cast<decltype(x.instrument)>(get());
x.cl_ord_id = static_cast<decltype(x.cl_ord_id)>(get());
x.cancel_on_disconnect = static_cast<decltype(x.cancel_on_disconnect)>(get());
return Command{x}; }
case CommandType::SetPhase: { SetPhase x;
x.ts = static_cast<decltype(x.ts)>(get());
x.instrument = static_cast<decltype(x.instrument)>(get());
x.phase = static_cast<decltype(x.phase)>(get());
return Command{x}; }
default: throw std::runtime_error("invalid journal command tag");
    }
}
} // namespace exsim::sim
#endif
