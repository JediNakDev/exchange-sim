#ifndef EXSIM_SIM_BOTS_BOT_CONTEXT_HPP
#define EXSIM_SIM_BOTS_BOT_CONTEXT_HPP

// What a bot needs from the server (server contract §5). The server's exchange
// context implements it and tests use a fake. Bots are templates over it, so
// there is no virtual dispatch.
//
//   submit(cmd)     Synchronous: the engine has processed `cmd` when it
//                   returns. The resulting event batch reaches the bots
//                   through on_events(), which the server calls after
//                   submit() returns. Bots also stay correct if a batch is
//                   delivered from inside submit(), because they record the
//                   expected outcome of every command before submitting it
//                   and never submit from on_events().
//   now()           caller-owned nanoseconds (same epoch as on_timer).
//   instrument(id)  static instrument configuration, nullptr if unknown.

#include "command.hpp"
#include "instrument.hpp"
#include "types.hpp"

#include <concepts>

namespace exsim::sim::bots {

template <typename Context>
concept BotContext = requires(Context& context, const Context& const_context, const Command& command,
                              InstrumentId instrument_id) {
    context.submit(command);
    { const_context.now() } -> std::convertible_to<Timestamp>;
    { const_context.instrument(instrument_id) } -> std::convertible_to<const InstrumentConfig*>;
};

}  // namespace exsim::sim::bots

#endif
