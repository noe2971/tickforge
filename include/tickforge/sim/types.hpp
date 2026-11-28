#pragma once
// Vocabulary of the market simulator (PROGRESS.md, Step 3, Rules G-K).
//
// A *shadow order* is an order of the simulated strategy. It is tracked inside
// the replayed real Nasdaq book (at an exact place in the queue) but never
// changes the real messages. Everything the strategy is told arrives through
// the events below, after the configured market-data latency.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "tickforge/book/types.hpp"

namespace tickforge::sim {

using book::OrderId;
using book::Price;
using book::Qty;
using book::Side;

using ShadowId = std::uint64_t;  // 1, 2, 3, ... in the order the strategy sends them; 0 = none
using Micros = std::int64_t;     // money in micro-dollars ($0.000001); a Price tick ($0.0001) is 100

inline constexpr Micros kMicrosPerTick = 100;
inline constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();
// Most markout horizons a run can measure (SimConfig::markout_horizons_ns).
inline constexpr std::size_t kMaxMarkoutHorizons = 8;

enum class FillModel : std::uint8_t {
    queue_aware,  // fills only when real volume reaches the order's place in the queue
    naive_touch,  // fills completely the first time the market trades at (or through) its price
};

enum class Liquidity : std::uint8_t { maker, taker };

// Per-share fees, Nasdaq style: resting (maker) orders earn a rebate, orders
// that take liquidity pay a fee. Defaults are illustrative ($0.0020 rebate,
// $0.0030 fee), not a quote of a specific Nasdaq tier.
struct FeeSchedule {
    Micros maker_rebate_per_share = 2000;  // received per share filled as maker (negative = a fee to add)
    Micros taker_fee_per_share = 3000;     // paid per share filled as taker
};

struct SimConfig {
    std::string symbol;                        // the stock to trade, e.g. "COLB"
    FillModel model = FillModel::queue_aware;
    std::uint64_t order_latency_ns = 0;        // strategy -> exchange (place, cancel, replace)
    std::uint64_t market_data_latency_ns = 0;  // exchange -> strategy (messages, fills, acknowledgements)
    FeeSchedule fees;
    std::uint64_t start_ns = 0;     // strategy start (on_start), nanoseconds after midnight ET
    std::uint64_t end_ns = kNever;  // run end: shadow orders cancelled, final P&L fixed
    std::uint64_t pnl_sample_interval_ns = 1'000'000'000;
    // Up to kMaxMarkoutHorizons values; FillRecord index h = position in this list.
    std::vector<std::uint64_t> markout_horizons_ns{100'000'000, 1'000'000'000, 10'000'000'000};
};

struct OrderFlags {
    bool post_only = false;  // rejected if it would cross the real book on arrival
    bool ioc = false;        // immediate-or-cancel: whatever does not fill on arrival is cancelled
};

enum class OrderStatus : std::uint8_t {
    pending,    // sent, not yet arrived at the exchange (or not yet acknowledged, as the strategy sees it)
    active,     // resting
    filled,
    cancelled,
    rejected,
    replaced,
};
const char* to_string(OrderStatus) noexcept;

enum class Reject : std::uint8_t {
    none = 0,
    bad_values,             // 0 shares or price <= 0
    post_only_would_cross,  // post-only order that would take liquidity
    self_cross,             // would cross the strategy's own resting order on the other side
    unknown_order,          // cancel/replace of an order that never became active
    too_late,               // cancel/replace arrived after the order was filled or cancelled
    after_end,              // sent after the run ended
};
inline constexpr std::size_t kRejectReasons = 7;
const char* to_string(Reject) noexcept;

// ---- What the strategy receives (after the market-data latency) -------------

// One real ITCH message of the strategy's stock, as delivered to the strategy.
struct MarketEvent {
    std::uint64_t ts = 0;  // exchange timestamp of the message
    char type = 0;         // 'A','F','E','C','X','D','U','P','Q','H'
    Side side = Side::buy; // the resting order's side (A/F/E/C/X/D/U/P)
    Price price = 0;       // A/F/U: order price; E/X/D: the order's price; C: execution price; P/Q: trade price
    Qty shares = 0;        // A/F/U: order size; E/C: executed; X: cancelled; D: shares removed; P/Q: traded
    OrderId ref = 0;       // order reference (U: the original)
    OrderId new_ref = 0;   // U only
    char flag = 0;         // C: printable Y/N; Q: cross type; H: trading state
};

enum class UpdateKind : std::uint8_t {
    accepted,   // the order arrived and is resting (or about to take liquidity)
    rejected,
    cancelled,  // fully cancelled (open_qty = 0) or partially (open_qty > 0)
    replaced,   // the old order is gone; replacement = new_id
    cancel_rejected,  // a cancel that could not be applied (reason: too_late or unknown_order); the order is unchanged
};
const char* to_string(UpdateKind) noexcept;

struct OrderUpdate {
    std::uint64_t exchange_ts = 0;
    ShadowId id = 0;
    UpdateKind kind = UpdateKind::accepted;
    Reject reason = Reject::none;
    Qty open_qty = 0;      // shares still open after this update
    Qty cancelled_qty = 0; // for cancelled
    ShadowId new_id = 0;   // for replaced
};

struct FillReport {
    std::uint64_t exchange_ts = 0;
    ShadowId id = 0;
    Side side = Side::buy;
    Price price = 0;
    Qty qty = 0;
    Liquidity liquidity = Liquidity::maker;
    Micros fee = 0;     // paid (positive) or received as a rebate (negative)
    Qty open_qty = 0;   // shares still open after this fill
};

// The strategy's own record of an order, built only from what it sent and
// what was delivered back to it.
struct KnownOrder {
    ShadowId id = 0;
    Side side = Side::buy;
    Price price = 0;
    Qty qty = 0;     // size sent
    Qty open = 0;    // as last reported
    Qty filled = 0;  // as reported so far
    OrderStatus status = OrderStatus::pending;
    OrderFlags flags;
};

// ---- What the simulator records (exchange truth, for analysis) --------------

struct FillRecord {
    FillReport report;
    char trigger = 0;  // message type that caused it; 'T' = taking liquidity on arrival
    std::int64_t mid2_before = 0;  // bid + ask (in $0.0001) just before the trigger; 0 = no two-sided book
    std::array<std::int64_t, kMaxMarkoutHorizons> mid2_after{};  // bid + ask after each markout horizon
    std::array<bool, kMaxMarkoutHorizons> markout_ok{};

    // side sign x (mid after horizon - mid before), dollars per share.
    double markout(std::size_t h) const noexcept {
        const double sign = report.side == Side::buy ? 1.0 : -1.0;
        return sign * static_cast<double>(mid2_after[h] - mid2_before) / 2.0 / 1e4;
    }
    // side sign x (mid after horizon - fill price), dollars per share.
    double markout_vs_price(std::size_t h) const noexcept {
        const double sign = report.side == Side::buy ? 1.0 : -1.0;
        return sign * (static_cast<double>(mid2_after[h]) / 2.0 - static_cast<double>(report.price)) / 1e4;
    }
};

struct PnlSample {
    std::uint64_t ts = 0;
    Micros cash = 0;
    std::int64_t inventory = 0;
    std::int64_t mid2 = 0;  // 0 = no two-sided book (mtm then uses the last valid mid)
    Micros mtm = 0;         // cash + inventory x mid
};

enum class DecisionKind : std::uint8_t { place, cancel, replace };

// Every action the strategy took, with the time it decided it.
struct Decision {
    std::uint64_t ts = 0;  // strategy time of the decision
    DecisionKind kind = DecisionKind::place;
    ShadowId id = 0;       // the order placed / cancelled / replaced
    ShadowId new_id = 0;   // replace: the replacement
    Side side = Side::buy;
    Price price = 0;
    Qty qty = 0;
    OrderFlags flags;
    friend bool operator==(const Decision& a, const Decision& b) noexcept {
        return a.ts == b.ts && a.kind == b.kind && a.id == b.id && a.new_id == b.new_id && a.side == b.side &&
               a.price == b.price && a.qty == b.qty && a.flags.post_only == b.flags.post_only &&
               a.flags.ioc == b.flags.ioc;
    }
};

struct SimStats {
    std::uint64_t orders_sent = 0;     // place + replace decisions
    std::uint64_t cancels_sent = 0;
    std::uint64_t accepted = 0;
    std::array<std::uint64_t, kRejectReasons> rejected{};
    std::uint64_t fills = 0;
    std::uint64_t filled_shares = 0;
    std::uint64_t placed_shares = 0;   // shares of accepted orders
    std::uint64_t maker_shares = 0;
    std::uint64_t taker_shares = 0;
    std::uint64_t bought_shares = 0;
    std::uint64_t sold_shares = 0;
    Micros rebates = 0;     // received
    Micros fees_paid = 0;   // paid
    std::int64_t max_inventory = 0;
    std::int64_t min_inventory = 0;
    // Executions or crossing adds that contradict the queue model (Rule H):
    // no fill is given for them.
    std::uint64_t anomaly_exec_with_ahead = 0;
    std::uint64_t anomaly_cross_add_with_ahead = 0;
    std::uint64_t ahead_underflow = 0;  // a real reduction larger than `ahead` (would mean a bookkeeping bug)
    std::uint64_t markouts_unavailable[kMaxMarkoutHorizons] = {};
    std::uint64_t strategy_callbacks = 0;
    std::uint64_t lookahead_violations = 0;  // callbacks that had seen data newer than allowed (must be 0)
    std::uint64_t messages = 0;              // ITCH messages read
    std::uint64_t stock_messages = 0;        // messages of the simulated stock delivered to the strategy
};

struct SimResult {
    bool symbol_found = false;
    std::uint16_t locate = 0;
    std::vector<FillRecord> fills;
    std::vector<PnlSample> pnl;
    std::vector<Decision> decisions;
    SimStats stats;
    std::uint64_t end_ts = 0;  // when the run ended
    Micros final_cash = 0;
    std::int64_t final_inventory = 0;
    std::int64_t final_mid2 = 0;
    Micros final_mtm = 0;
};

}  // namespace tickforge::sim
