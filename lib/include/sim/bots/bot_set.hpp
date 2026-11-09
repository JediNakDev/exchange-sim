#ifndef EXSIM_SIM_BOTS_BOT_SET_HPP
#define EXSIM_SIM_BOTS_BOT_SET_HPP
#include "market_maker.hpp"
#include "noise_taker.hpp"
#include <array>
#include <optional>
namespace exsim::sim::bots {
// add() is setup-only. One session per bot; duplicate sessions fail visibly.
template <std::size_t N> class BotSet {
public:
    MarketMaker* add(const MarketMakerConfig& config) {
        if (!available(config)) return nullptr;
        for (auto& bot : makers_) if (!bot) { ++size_; return &bot.emplace(config); }
        return nullptr;
    }
    NoiseTaker* add(const NoiseTakerConfig& config) {
        if (!available(config)) return nullptr;
        for (auto& bot : takers_) if (!bot) { ++size_; return &bot.emplace(config); }
        return nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    void set_enabled(bool enabled) noexcept {
        for (auto& bot : makers_) if (bot) bot->set_enabled(enabled);
        for (auto& bot : takers_) if (bot) bot->set_enabled(enabled);
    }
    void on_events(std::span<const Event> events) noexcept {
        for (auto& bot : makers_) if (bot) bot->on_events(events);
        for (auto& bot : takers_) if (bot) bot->on_events(events);
    }
    template <BotContext Context> void on_timer(Timestamp now, Context& context) {
        for (auto& bot : makers_) if (bot) bot->on_timer(now, context);
        for (auto& bot : takers_) if (bot) bot->on_timer(now, context);
    }
private:
    bool available(const BotConfig& config) const noexcept {
        if (size_ == N || !is_valid_bot_config(config)) return false;
        for (const auto& bot : makers_) if (bot && bot->config().session_id == config.session_id) return false;
        for (const auto& bot : takers_) if (bot && bot->config().session_id == config.session_id) return false;
        return true;
    }
    std::array<std::optional<MarketMaker>, N> makers_{};
    std::array<std::optional<NoiseTaker>, N> takers_{};
    std::size_t size_ = 0;
};
} // namespace exsim::sim::bots
#endif
