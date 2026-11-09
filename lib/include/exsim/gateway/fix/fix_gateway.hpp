#ifndef EXSIM_GATEWAY_FIX_FIX_GATEWAY_HPP
#define EXSIM_GATEWAY_FIX_FIX_GATEWAY_HPP

// FIX 4.4 order-entry gateway (gateway mapping 3, server contract 2).
//   FixUserDirectory / FixUserState   day lifetime, one state per FIX user
//   FixConnection                     one TCP connection, exchange side
//   FixClient                         client side (tests, tools, benchmarks)

#include "exsim/gateway/fix/fix_client.hpp"
#include "exsim/gateway/fix/fix_common.hpp"
#include "exsim/gateway/fix/fix_connection.hpp"
#include "exsim/gateway/fix/fix_session_day_state.hpp"
#include "exsim/gateway/fix/fix_session_layer.hpp"
#include "exsim/gateway/fix/fix_user_state.hpp"

#endif
