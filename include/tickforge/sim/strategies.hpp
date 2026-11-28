#pragma once
// Two built-in strategies.
//
// JoinBestQuoter — the Rule L demonstration strategy: one post-only order of
//   `size` shares at the best bid and one at the best ask (as the strategy
//   sees them); when that side's best price changes the order is replaced at
//   the new best. No buying at position >= +max_inventory, no selling at
//   <= -max_inventory. After a rejection it waits for the next market event
//   before quoting that side again (so a stale view cannot cause a loop).
//
// ScheduledOrders — sends a fixed list of actions at fixed times and never
//   reacts to anything. Because its orders do not depend on fills, it is what
//   the naive-vs-queue-aware comparison and the latency tests use.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "tickforge/sim/strategy.hpp"

namespace tickforge::sim {

class JoinBestQuoter : public Strategy {
public:
    explicit JoinBestQuoter(Qty size = 100, std::int64_t max_inventory = 500) : size_(size), max_inv_(max_inventory) {}

    void on_start(Context& c) override { requote(c); }
    void on_market(Context& c, const MarketEvent&) override;
    void on_order(Context& c, const OrderUpdate& u) override;
    void on_fill(Context& c, const FillReport&) override { requote(c); }

private:
    struct Slot {
        ShadowId id = 0;
        Price price = 0;
        bool cancelling = false;
        bool blocked = false;  // rejected: wait for the next market event
    };
    void requote(Context& c);
    void requote_side(Context& c, Side s);

    Qty size_;
    std::int64_t max_inv_;
    Slot slots_[2];
};

struct ScheduledAction {
    std::uint64_t at_ns = 0;
    DecisionKind kind = DecisionKind::place;
    std::size_t target = 0;  // cancel / replace: index (in the plan) of the earlier place or replace it acts on
    Side side = Side::buy;
    Price price = 0;
    Qty qty = 0;             // place / replace: size; cancel: shares to cancel (0 = all)
    OrderFlags flags;
};

class ScheduledOrders : public Strategy {
public:
    explicit ScheduledOrders(std::vector<ScheduledAction> plan) : plan_(std::move(plan)), ids_(plan_.size(), 0) {}

    void on_start(Context& c) override;
    void on_timer(Context& c, std::uint64_t timer_id) override;

    // Order id created by plan step i (0 for cancels, or if not sent yet).
    const std::vector<ShadowId>& ids() const noexcept { return ids_; }

private:
    std::vector<ScheduledAction> plan_;
    std::vector<ShadowId> ids_;
    std::unordered_map<std::uint64_t, std::size_t> timer_to_step_;
};

}  // namespace tickforge::sim
