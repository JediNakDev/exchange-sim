#ifndef EXSIM_GATEWAY_FIX_FIX_GATEWAY_HPP
#define EXSIM_GATEWAY_FIX_FIX_GATEWAY_HPP

// FIX 4.4 order-entry gateway, exchange side.
//   FixUserDirectory / FixUserState   day lifetime, one state per FIX user
//   FixConnection                     one TCP connection
// The test client (FixClient) is test/exsim/support/clients/fix_client.hpp.

#include "gateway/fix/fix_common.hpp"
#include "gateway/fix/fix_connection.hpp"
#include "gateway/fix/fix_session_day_state.hpp"
#include "gateway/fix/fix_session_layer.hpp"
#include "gateway/fix/fix_user_state.hpp"

#endif
