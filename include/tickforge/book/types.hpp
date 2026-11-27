#pragma once
// Vocabulary shared by both order book engines (FastBook and ReferenceBook).
//
// Both engines expose exactly the same operations and return exactly the same
// values, which is what lets the differential fuzz test run one random stream
// through both and demand identical answers after every event.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace tickforge::book {

using OrderId = std::uint64_t;

// A price in ticks of $0.0001. This is exactly ITCH's Price(4) integer
// ($1.2345 -> 12345), so prices are never converted or rounded. Signed so a
// bad price (<= 0) can be rejected instead of wrapping around.
using Price = std::int64_t;

// Shares in one order (ITCH uses 4-byte share counts). Totals at a level are
// 64-bit because many orders can sum past 2^32.
using Qty = std::uint32_t;

enum class Side : std::uint8_t { buy = 0, sell = 1 };

constexpr Side opposite(Side s) noexcept { return s == Side::buy ? Side::sell : Side::buy; }
constexpr int index(Side s) noexcept { return static_cast<int>(s); }

// True if price a is strictly better than b for a resting order on side s:
// higher is better for bids, lower is better for asks.
constexpr bool better(Side s, Price a, Price b) noexcept { return s == Side::buy ? a > b : a < b; }

// True if an incoming order on side `taker` with limit `limit` may trade
// against a resting order priced at `resting`.
constexpr bool crosses(Side taker, Price limit, Price resting) noexcept {
    return taker == Side::buy ? resting <= limit : resting >= limit;
}

enum class Status : std::uint8_t {
    ok,
    unknown_order,    // the id is not a live order in this book
    duplicate_order,  // the id is already a live order in this book
    zero_quantity,    // 0 shares requested
    too_many_shares,  // execute/cancel more shares than the order has left
    bad_price,        // price <= 0
};
inline constexpr std::size_t kStatusCount = 6;

constexpr const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::ok: return "ok";
        case Status::unknown_order: return "unknown_order";
        case Status::duplicate_order: return "duplicate_order";
        case Status::zero_quantity: return "zero_quantity";
        case Status::too_many_shares: return "too_many_shares";
        case Status::bad_price: return "bad_price";
    }
    return "?";
}

// One trade produced by the matching engine.
struct Fill {
    OrderId maker = 0;  // the resting order
    OrderId taker = 0;  // the incoming order
    Price price = 0;    // always the maker's (resting) price
    Qty qty = 0;
    friend bool operator==(const Fill&, const Fill&) = default;
};

// Result of submit_limit / submit_market.
struct SubmitResult {
    Status status = Status::ok;
    Qty filled = 0;  // shares traded immediately
    Qty rested = 0;  // shares left resting on the book (limit orders only)
    friend bool operator==(const SubmitResult&, const SubmitResult&) = default;
};

// Result of execute(): what the executed order looked like.
struct ExecResult {
    Status status = Status::ok;
    Side side = Side::buy;
    Price price = 0;    // the order's resting price
    Qty remaining = 0;  // shares left afterwards; 0 means the order is gone
    friend bool operator==(const ExecResult&, const ExecResult&) = default;
};

// Aggregate view of one price level.
struct LevelView {
    Price price = 0;
    std::uint64_t shares = 0;  // sum of the remaining shares of its orders
    std::uint32_t orders = 0;  // number of orders queued at this price
    friend bool operator==(const LevelView&, const LevelView&) = default;
};

// View of one live order.
struct OrderView {
    OrderId id = 0;
    Side side = Side::buy;
    Price price = 0;
    Qty qty = 0;            // remaining shares
    bool at_front = false;  // first in its level's time-priority queue
    friend bool operator==(const OrderView&, const OrderView&) = default;
};

// Something that receives fills: any callable taking `const Fill&`.
template <typename F>
concept FillSink = std::invocable<F&, const Fill&>;

// A sink that drops fills (replay mode, benchmarks).
struct IgnoreFills {
    void operator()(const Fill&) const noexcept {}
};

// The interface both engines implement. Replay mode (add / execute / cancel /
// remove / replace) applies what an exchange feed says happened and never
// matches; matching mode (submit_limit / submit_market) runs price-time
// priority matching itself. Both modes work on the same book.
template <typename B>
concept OrderBook = requires(B b, const B cb, OrderId id, Side s, Price p, Qty q, LevelView* out, std::size_t n,
                             IgnoreFills sink) {
    // replay mode
    { b.add(id, s, p, q) } -> std::same_as<Status>;
    { b.execute(id, q) } -> std::same_as<ExecResult>;
    { b.cancel(id, q) } -> std::same_as<Status>;
    { b.remove(id) } -> std::same_as<Status>;
    { b.replace(id, id, p, q) } -> std::same_as<Status>;
    // matching mode
    { b.submit_limit(id, s, p, q, sink) } -> std::same_as<SubmitResult>;
    { b.submit_market(id, s, q, sink) } -> std::same_as<SubmitResult>;
    // queries
    { cb.best(s) } -> std::same_as<std::optional<LevelView>>;
    { cb.depth(s, out, n) } -> std::same_as<std::size_t>;
    { cb.find(id) } -> std::same_as<std::optional<OrderView>>;
    { cb.order_count() } -> std::same_as<std::size_t>;
    { cb.level_count(s) } -> std::same_as<std::size_t>;
    b.reserve(n, n);
};

}  // namespace tickforge::book
