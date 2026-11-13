#ifndef EXSIM_SERVER_CORE_ADMIN_HPP
#define EXSIM_SERVER_CORE_ADMIN_HPP

// Runs admin console commands (admin.hpp) against an ExchangeCore: status
// text, phase changes, book printing, market-data and user fault injection,
// shutdown. Operator path: allocates freely, never runs during dispatch.
//
//   std::string reply;
//   CoreAdmin(core).execute_line("fault user ALICE reject 2", reply);
//
// Replies are '\n'-terminated lines ending with "OK" or "ERR <message>".

#include "md/publisher.hpp"
#include "server/admin.hpp"
#include "server/config.hpp"
#include "server/core/admin_text.hpp"
#include "server/core/policies.hpp"
#include "server/core/user_faults.hpp"
#include "server/exchange_core.hpp"
#include "sim/packet_faults.hpp"
#include "sim/price_text.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace exsim::server {

template <class Core>
class CoreAdmin {
public:
    explicit CoreAdmin(Core& core) noexcept : core_(core) {}

    void execute_line(std::string_view line, std::string& reply) {
        AdminCommand command;
        std::string error;
        if (!parse_admin(line, command, error)) {
            append_error(reply, error);
            return;
        }
        execute(command, reply);
    }

    // Appends the reply lines, ending with "OK" or "ERR <message>".
    void execute(const AdminCommand& command, std::string& reply) {
        switch (command.op) {
            case AdminOp::Help:
                reply.append(admin_help());
                append_ok(reply);
                return;
            case AdminOp::Status: append_status(reply); return;
            case AdminOp::Phase: execute_phase(command, reply); return;
            case AdminOp::Book: {
                const InstrumentId id = admin_instrument(command.symbol);
                if (id == 0) {
                    append_error(reply, "unknown instrument " + command.symbol);
                    return;
                }
                append_book(reply, core_.engine_, id, command.count);
                return;
            }
            case AdminOp::FaultMdDrop:
            case AdminOp::FaultMdDup:
            case AdminOp::FaultMdReorder:
            case AdminOp::FaultMdRate:
            case AdminOp::FaultMdOff: execute_market_data_fault(command, reply); return;
            case AdminOp::FaultUserReject:
            case AdminOp::FaultUserDelay:
            case AdminOp::FaultUserDropOut:
            case AdminOp::FaultUserDisconnect:
            case AdminOp::FaultUserOff: execute_user_fault(command, reply); return;
            case AdminOp::Shutdown:
                if (core_.shutdown_) {
                    append_line(reply, "already shut down");
                } else {
                    core_.shutdown();
                    append_line(reply, core_.shutdown_close_result_ == SubmitResult::Applied
                                           ? "all instruments closed"
                                           : "SetPhase Closed not applied: " +
                                                 std::string(to_string(core_.shutdown_close_result_)));
                }
                append_ok(reply);
                return;
        }
        append_error(reply, "unsupported admin command");
    }

private:
    using MarketData = std::remove_reference_t<decltype(std::declval<Core&>().market_data_)>;
    using UserRuntime = typename Core::UserRuntime;

    [[nodiscard]] InstrumentId admin_instrument(std::string_view symbol) const noexcept {
        if (symbol.empty() || symbol.size() > Symbol{}.chars.size()) {
            return 0;
        }
        return core_.engine_.find_instrument(Symbol(symbol));
    }

    void execute_phase(const AdminCommand& command, std::string& reply) {
        if (core_.shutdown_) {
            append_error(reply, "exchange is shut down");
            return;
        }
        InstrumentId target = 0;
        if (command.symbol != "*") {
            target = admin_instrument(command.symbol);
            if (target == 0) {
                append_error(reply, "unknown instrument " + command.symbol);
                return;
            }
        }
        const std::size_t count = core_.engine_.instrument_count();
        std::vector<Phase> before(count + 1, Phase::Closed);
        for (std::size_t id = 1; id <= count; ++id) {
            before[id] = core_.engine_.phase(static_cast<InstrumentId>(id));
        }
        SetPhase set_phase{};
        set_phase.instrument = target;
        set_phase.phase = command.phase;
        const SubmitResult result = core_.submit(Command{set_phase});
        if (result != SubmitResult::Applied) {
            append_error(reply, std::string("phase not applied: ") + std::string(to_string(result)));
            return;
        }
        for (std::size_t id = 1; id <= count; ++id) {
            if (target != 0 && id != target) {
                continue;
            }
            const auto instrument_id = static_cast<InstrumentId>(id);
            std::string line(core_.engine_.instrument(instrument_id)->symbol.view());
            line += ' ';
            line += to_string(before[id]);
            line += " -> ";
            line += to_string(core_.engine_.phase(instrument_id));
            append_line(reply, line);
        }
        append_ok(reply);
    }

    void execute_market_data_fault(const AdminCommand& command, std::string& reply) {
        if constexpr (MarketDataPacketFaults<MarketData>) {
            sim::PacketFaultInjector& injector = core_.market_data_.packet_faults();
            switch (command.op) {
                case AdminOp::FaultMdDrop: injector.drop_next(command.count); break;
                case AdminOp::FaultMdDup: injector.duplicate_next(command.count); break;
                case AdminOp::FaultMdReorder: {
                    sim::PacketFaultConfig with_delay = injector.config();
                    with_delay.reorder_delay = command.delay;
                    injector.configure(with_delay);
                    injector.reorder_next(command.count);
                    break;
                }
                case AdminOp::FaultMdRate: injector.configure(command.rate); break;
                case AdminOp::FaultMdOff:
                    if constexpr (MarketDataPacketFaultReset<MarketData>) {
                        core_.market_data_.reset_packet_faults();
                    } else {
                        injector.configure(sim::PacketFaultConfig{});
                        if (!injector.idle()) {
                            append_line(reply, "note: pending one-shot faults or held packets remain "
                                               "(market data exposes no reset_packet_faults)");
                        }
                    }
                    break;
                default: break;
            }
            append_line(reply, "md faults " + describe_packet_faults(core_.market_data_.packet_faults()));
            append_ok(reply);
        } else {
            (void)command;
            append_error(reply, "market data exposes no packet fault injector");
        }
    }

    void execute_user_fault(const AdminCommand& command, std::string& reply) {
        UserRuntime* user = user_named(command.user);
        if (user == nullptr) {
            append_error(reply, "unknown user " + command.user);
            return;
        }
        UserFaultState& faults = user->faults;
        switch (command.op) {
            case AdminOp::FaultUserReject: faults.reject_next_new_orders = command.count; break;
            case AdminOp::FaultUserDelay: faults.inbound_delay_ns = std::uint64_t{command.delay} * 1000; break;
            case AdminOp::FaultUserDropOut: faults.drop_next_outbound = command.count; break;
            case AdminOp::FaultUserDisconnect:
                if (!user->bound) {
                    append_error(reply, "user " + command.user + " is not connected");
                    return;
                }
                if (!faults.disconnect_requested) {
                    faults.disconnect_requested = true;
                    ++core_.disconnect_requests_pending_;
                }
                break;
            case AdminOp::FaultUserOff:
                core_.clear_disconnect_request(*user);
                faults = UserFaultState{};
                break;
            default: break;
        }
        append_line(reply, "user " + command.user + " session=" + std::to_string(user->config->session_id) + " " +
                               describe_user_faults(faults));
        append_ok(reply);
    }

    // Linear in the users: operator path only.
    [[nodiscard]] UserRuntime* user_named(std::string_view username) noexcept {
        for (UserRuntime& user : core_.users_) {
            if (user.config->username == username) {
                return &user;
            }
        }
        return nullptr;
    }

    void append_status(std::string& reply) {
        const ServerConfig& config = core_.config_;
        const CoreCounters& counters = core_.counters_;
        const auto& engine = core_.engine_;
        append_line(reply, "exchange " + config.exchange.name + " shutdown=" + (core_.shutdown_ ? "yes" : "no"));
        append_line(reply, "trading_day start=" + format_day_time(core_.day_start_) +
                               " end=" + format_day_time(core_.day_end_) +
                               " last_command_ts=" + std::to_string(core_.last_command_ts_));
        append_line(reply, "connections bound=" + std::to_string(core_.bound_connection_count_) +
                               " users=" + std::to_string(core_.users_.size()));
        for (const UserRuntime& user : core_.users_) {
            std::string line = "user " + user.config->username + " session=" + std::to_string(user.config->session_id) +
                               " protocol=" + std::string(to_string(user.config->protocol));
            line += std::string(" bound=") + (user.bound ? "yes connection=" + std::to_string(user.connection) : "no");
            line += " " + describe_user_faults(user.faults);
            append_line(reply, line);
        }
        append_line(reply, "orders live=" + std::to_string(engine.live_order_count()));
        for (std::size_t id = 1; id <= engine.instrument_count(); ++id) {
            const auto instrument_id = static_cast<InstrumentId>(id);
            std::string line = "instrument " + std::string(engine.instrument(instrument_id)->symbol.view()) +
                               " id=" + std::to_string(id) + " phase=" + std::string(to_string(engine.phase(instrument_id)));
            const auto bid = engine.best_bid(instrument_id);
            const auto ask = engine.best_ask(instrument_id);
            const auto last = engine.last_trade_price(instrument_id);
            line += " bid=" + (bid ? sim::format_price(*bid, 0) : std::string("-"));
            line += " ask=" + (ask ? sim::format_price(*ask, 0) : std::string("-"));
            line += " last=" + (last ? sim::format_price(*last, 0) : std::string("-"));
            append_line(reply, line);
        }
        const MarketData& market_data = core_.market_data_;
        if constexpr (MarketDataSequence<MarketData>) {
            append_line(reply, "md next_sequence=" + std::to_string(market_data.next_sequence()));
        } else {
            append_line(reply, "md next_sequence=unavailable");
        }
        if constexpr (MarketDataPacketFaults<MarketData>) {
            append_line(reply, "md faults " + describe_packet_faults(core_.market_data_.packet_faults()));
        } else {
            append_line(reply, "md faults unavailable");
        }
        std::string market_data_state_line = "md state=";
        if constexpr (MarketDataPublisherState<MarketData>) {
            market_data_state_line += std::string(md_state_text(market_data.state()));
            market_data_state_line += " stop_reason=" + std::string(md_status_text(market_data.stop_reason()));
        } else {
            market_data_state_line += "unavailable";
        }
        market_data_state_line += " failed_batches=" + std::to_string(counters.market_data_failed_batches) +
                                  " last_failure=" + std::string(md_status_text(counters.market_data_last_failure));
        append_line(reply, market_data_state_line);
        append_line(reply, "journal state=" + std::string(to_string(core_.journal_state())) +
                               (config.journal_path.empty() ? std::string() : " path=" + config.journal_path) +
                               " records=" + std::to_string(counters.journal_records) +
                               " failures=" + std::to_string(counters.journal_failures) +
                               " on_failure=" + std::string(to_string(config.journal_failure_policy)));
        append_line(reply, "commands applied=" + std::to_string(counters.commands_applied) +
                               " outside_trading_day=" + std::to_string(counters.commands_outside_trading_day) +
                               " after_shutdown=" + std::to_string(counters.commands_after_shutdown) +
                               " output_capacity_failures=" + std::to_string(counters.output_capacity_failures) +
                               " deferred=" + std::to_string(counters.commands_deferred) +
                               " deferred_overflows=" + std::to_string(counters.deferred_queue_overflows));
        append_ok(reply);
    }

    [[nodiscard]] static std::string_view md_state_text(md::PublisherState state) noexcept {
        switch (state) {
            case md::PublisherState::Setup: return "setup";
            case md::PublisherState::Live: return "live";
            case md::PublisherState::Stopped: return "stopped";
            case md::PublisherState::Ended: return "ended";
        }
        return "?";
    }

    [[nodiscard]] static std::string_view md_status_text(md::PublishStatus status) noexcept {
        switch (status) {
            case md::PublishStatus::Ok: return "none";
            case md::PublishStatus::InvalidConfig: return "invalid_config";
            case md::PublishStatus::InvalidInstrument: return "invalid_instrument";
            case md::PublishStatus::DuplicateInstrument: return "duplicate_instrument";
            case md::PublishStatus::NotAllowedInState: return "not_allowed_in_state";
            case md::PublishStatus::TimestampOutsideDay: return "timestamp_outside_day";
            case md::PublishStatus::UnknownInstrument: return "unknown_instrument";
            case md::PublishStatus::UnrepresentableField: return "unrepresentable_field";
        }
        return "?";
    }

    Core& core_;
};

}  // namespace exsim::server

#endif
