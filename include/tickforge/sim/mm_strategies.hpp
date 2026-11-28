#pragma once
// Step 4 market-making strategies (PROGRESS.md, Rules N and O).
//
// TwoSidedQuoter — shared mechanics: at most one post-only order per side,
//   replaced (losing queue priority) when that side's target price changes;
//   sizes clipped so the reported position stays inside +/- max_inventory;
//   no quotes before quote_start, with a one-sided book, or when the trading
//   state is not 'T'. After a rejection a side waits for the next market
//   event. A derived class only decides the target prices.
//
// SymmetricInsideQuoter — Rule N: one tick inside the spread on both sides
//   when the spread is at least 3 ticks, else join the best on both sides.
//
// AvellanedaStoikov — Rule O: Avellaneda & Stoikov (2008) reservation price
//   and spread, with sigma^2 and k estimated only from data already delivered.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "tickforge/sim/strategy.hpp"

namespace tickforge::sim {

inline constexpr Price kCent = 100;  // one tick ($0.01) in ITCH price units ($0.0001)

struct QuoterParams {
    Qty size = 100;
    std::int64_t max_inventory = 500;
    std::uint64_t quote_start_ns = 0;  // no orders before this strategy time
};

// Counts over every order the strategy sent (place or replace), comparing its
// price with the best price on its side as the strategy saw it.
struct QuoteStats {
    std::uint64_t quotes = 0;
    std::uint64_t improved = 0;  // better than the best price on its side
    std::uint64_t at_best = 0;
    std::uint64_t behind = 0;
};

class TwoSidedQuoter : public Strategy {
public:
    explicit TwoSidedQuoter(QuoterParams p) : p_(p) {}

    void on_start(Context& c) override;
    void on_market(Context& c, const MarketEvent& m) override;
    void on_order(Context& c, const OrderUpdate& u) override;
    void on_fill(Context& c, const FillReport&) override { requote(c); }
    void on_timer(Context& c, std::uint64_t id) override;

    const QuoteStats& quote_stats() const noexcept { return stats_; }
    const QuoterParams& params() const noexcept { return p_; }

protected:
    struct Targets {
        std::optional<Price> bid;
        std::optional<Price> ask;
    };
    // Called only with a two-sided book, in trading state, at or after quote_start.
    virtual Targets targets(Context& c) = 0;
    // Hooks for estimators; called before the requote of the same callback.
    virtual void observe_start(Context&) {}
    virtual void observe_market(Context&, const MarketEvent&) {}
    virtual void observe_timer(Context&, std::uint64_t /*timer_id*/) {}

private:
    struct Slot {
        ShadowId id = 0;
        Price price = 0;
        bool cancelling = false;
        bool blocked = false;
    };
    void requote(Context& c);
    void requote_side(Context& c, Side s, std::optional<Price> target, Price seen_best);

    QuoterParams p_;
    Slot slots_[2];
    std::uint64_t start_timer_ = 0;
    QuoteStats stats_;
};

class SymmetricInsideQuoter final : public TwoSidedQuoter {
public:
    explicit SymmetricInsideQuoter(QuoterParams p) : TwoSidedQuoter(p) {}

    // Rule N prices from the best bid b and best ask a.
    static Targets prices(Price b, Price a) noexcept;

protected:
    Targets targets(Context& c) override;
};

struct AsParams {
    double gamma = 4.56e-5;                // risk aversion, per dollar
    std::uint64_t horizon_end_ns = 0;      // T (15:55:00 in the study)
    std::uint64_t sample_interval_ns = 1'000'000'000;  // mid sampled every second (tests use less)
    std::size_t sigma_window = 1800;       // mid changes kept (30 minutes of 1-second changes)
    std::size_t min_sigma_changes = 60;    // no quotes before this many
    std::size_t min_executions = 20;       // no quotes before this many
};

// One Avellaneda-Stoikov quote, rounded to cents and made post-only.
struct AsQuote {
    double reservation = 0;  // $
    double half_spread = 0;  // $ (at least half a tick)
    std::optional<Price> bid;
    std::optional<Price> ask;
};

// Pure formula (Rule O): mid in $, q in shares, var in $^2/s, mean_dist = 1/k
// in $, tau = T - t in seconds, gamma per $; best_bid/best_ask in ITCH units.
AsQuote as_quote(double mid, std::int64_t q, double var, double mean_dist, double tau, double gamma,
                 Price best_bid, Price best_ask) noexcept;

class AvellanedaStoikov final : public TwoSidedQuoter {
public:
    AvellanedaStoikov(QuoterParams p, AsParams a) : TwoSidedQuoter(p), a_(a) {}

    // Current estimates (only from delivered data).
    std::size_t sigma_changes() const noexcept { return count_; }
    double variance_per_second() const noexcept;  // $^2 per second; 0 with no changes
    std::size_t executions() const noexcept { return n_exec_; }
    double mean_exec_distance() const noexcept;   // $; 0 with no executions
    bool ready() const noexcept { return count_ >= a_.min_sigma_changes && n_exec_ >= a_.min_executions; }

protected:
    Targets targets(Context& c) override;
    void observe_start(Context& c) override;
    void observe_market(Context& c, const MarketEvent& m) override;
    void observe_timer(Context& c, std::uint64_t id) override;

private:
    static std::int64_t mid2(const Context& c) noexcept;  // bid + ask, 0 if one-sided

    AsParams a_;
    // sigma: ring buffer of squared changes of mid2 (exact integers).
    std::vector<std::int64_t> sq_;
    std::size_t head_ = 0, count_ = 0;
    std::int64_t sum_sq_ = 0;
    std::int64_t last_sample_ = 0;  // 0 = no valid previous sample
    std::uint64_t sample_timer_ = 0;
    // k: |2 x price - mid2| summed over displayed executions (units $0.00005).
    std::int64_t sum_dist2_ = 0;
    std::size_t n_exec_ = 0;
    std::int64_t prev_mid2_ = 0;  // mid2 after the previous delivered message
};

}  // namespace tickforge::sim
