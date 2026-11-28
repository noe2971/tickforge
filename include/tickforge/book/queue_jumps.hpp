#pragma once
// QueueJumpAnalyzer: explains the `E` executions that hit an order which was
// not first in its price level's queue (Step 2 found 7,056 of 8,096,995).
//
// For every such "queue jump" it records the orders that were ahead of the
// executed order (the "skipped" orders) and follows them:
//
//   1. Same timestamp. Did every skipped order also execute (or leave the
//      book) in messages carrying the *same nanosecond timestamp*? One
//      aggressive order that sweeps a level produces a burst of `E` messages
//      with one timestamp; if Nasdaq publishes them in an order different
//      from queue order, the set of orders that traded is still FIFO, only
//      the publication order is not.
//   2. Cross. Was there a Cross Trade (`Q`) for the stock at that timestamp
//      (opening / closing / halt cross executions)?
//   3. Otherwise the skipped order really stayed in the book while an order
//      behind it traded. What happened to it next (executed, cancelled,
//      deleted, replaced, still there at the end) and how long after?
//
// This is analysis code: it looks ahead in the feed (to the end of the
// timestamp, and to each skipped order's next event). It is never used by the
// simulator, which must not see the future.

#include <array>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "tickforge/book/fast_book.hpp"
#include "tickforge/book/itch_replay.hpp"
#include "tickforge/itch/locate_table.hpp"
#include "tickforge/itch/messages.hpp"

namespace tickforge::book {

// What happened to a skipped order: its first event, or "still in the book".
enum class Fate : std::uint8_t {
    none = 0,       // no event (yet)
    executed_e,     // E
    executed_c,     // C
    cancelled,      // X (partial cancel)
    deleted,        // D
    replaced,       // U (it left the book under its reference)
    end_of_day,     // still in the book when the file ended
};
inline constexpr std::size_t kFates = 7;
const char* to_string(Fate) noexcept;

// How a queue jump resolved by the end of its timestamp.
enum class JumpClass : std::uint8_t {
    all_ahead_executed_same_ts = 0,  // every skipped order fully executed at the same timestamp
    all_ahead_gone_same_ts,          // every skipped order gone at the same timestamp, some by X/D/U
    some_ahead_remain,               // at least one skipped order still rests after that timestamp
};
inline constexpr std::size_t kJumpClasses = 3;
const char* to_string(JumpClass) noexcept;

struct OrderInfo {
    std::uint16_t locate = 0;
    char kind = 'A';               // 'A', 'F' or 'U' (created by a replace)
    std::uint32_t mpid = 0;        // packed 4 ASCII chars for F, else 0
    std::uint64_t add_ts = 0;
    std::uint32_t added_shares = 0;
    std::uint32_t partial_execs = 0;
    std::uint32_t partial_cancels = 0;
    std::int64_t opposite_gap = -1;  // opposite best minus own price (ticks of $0.0001, towards the other side); -1 = no opposite side
    bool late = false;             // its reference number is below one already seen in the feed (published "late")
    std::uint64_t late_by_ns = 0;  // if late: add time minus the time the first larger reference was seen
};

struct SkippedOrder {
    OrderId id = 0;
    Qty shares = 0;          // shares it had when it was skipped
    OrderInfo info;
    // Same timestamp:
    bool gone_same_ts = false;
    bool gone_by_execution = false;  // gone, and only through E/C
    bool other_same_ts = false;      // an X/D/U touched it at the same timestamp
    // First event after the timestamp (only if it was still in the book):
    Fate next = Fate::none;
    std::uint64_t next_dt_ns = 0;
    int next_c_rel = 0;  // for C: 0 equal, 1 more aggressive than displayed (better for the other side), 2 less
};

struct QueueJump {
    std::uint64_t ts = 0;
    std::uint16_t locate = 0;
    OrderId ref = 0;
    Side side = Side::buy;
    Price price = 0;
    std::uint32_t position = 0;      // 1-based position of the executed order in its queue
    std::uint64_t ahead_shares = 0;
    Qty executed = 0;
    bool fully_executed = false;
    bool market_hours = false;
    bool in_cross = false;           // a Q for this stock carried the same timestamp
    bool hidden_trade_same_ts = false;  // a P for this stock carried the same timestamp
    OrderInfo jumper;
    std::vector<SkippedOrder> ahead;
    JumpClass cls = JumpClass::some_ahead_remain;
    bool classified = false;
};

class QueueJumpAnalyzer : public itch::NullHandler {
public:
    using itch::NullHandler::on;

    QueueJumpAnalyzer();

    void on(const itch::SystemEvent& m);
    void on(const itch::StockDirectory& m);
    void on(const itch::AddOrder& m);
    void on(const itch::AddOrderMpid& m);
    void on(const itch::OrderExecuted& m);
    void on(const itch::OrderExecutedWithPrice& m);
    void on(const itch::OrderCancel& m);
    void on(const itch::OrderDelete& m);
    void on(const itch::OrderReplace& m);
    void on(const itch::Trade& m);
    void on(const itch::CrossTrade& m);
    template <typename M>
    void on(const M& m) {
        tick(m.header.timestamp_ns);
    }

    // Call once after the last message.
    void finish();

    const std::vector<QueueJump>& jumps() const noexcept { return jumps_; }
    std::uint64_t executions() const noexcept { return e_total_; }
    // [first in arrival order? no/yes][smallest reference number at its level? no/yes]
    std::uint64_t e_order(bool front, bool min_ref) const noexcept { return e_order_[front][min_ref]; }
    std::uint64_t late_adds() const noexcept { return late_adds_; }
    std::uint64_t front_executions() const noexcept { return e_front_; }

    void write_markdown(std::ostream& out, const std::string& input_name) const;

private:
    struct Watch {
        std::uint32_t jump;
        std::uint32_t idx;
    };

    void tick(std::uint64_t ts);
    void end_timestamp();
    void add(const itch::Header& h, OrderId ref, char side, Qty shares, Price price, char kind, std::uint32_t mpid);
    // An event on order `ref`: kind, shares removed (0 for D/U = all), C price.
    void touch(const itch::Header& h, OrderId ref, Fate kind, Qty shares, Price c_price);
    void forget(OrderId ref);
    void note_ref(OrderInfo& info, OrderId ref, std::uint64_t ts);

    BookSet<FastBook> books_;
    itch::LocateTable locates_;
    std::unordered_map<OrderId, OrderInfo> info_;
    std::unordered_map<OrderId, std::vector<Watch>> watch_;
    std::vector<QueueJump> jumps_;
    std::vector<std::uint32_t> open_jumps_;  // jumps at the current timestamp, not yet classified
    std::vector<std::uint16_t> cross_locates_;   // Q seen at the current timestamp
    std::vector<std::uint16_t> hidden_locates_;  // P seen at the current timestamp
    std::uint64_t cur_ts_ = 0;
    std::uint64_t open_ts_ = 0;  // System Event 'Q' (start of market hours); 0 = not seen yet
    bool market_open_ = false;
    bool finished_ = false;

    // Reference-number order, per stock. ref_clock_[locate] holds (ref, ts)
    // each time that stock's largest reference seen so far increased, at most
    // one entry per millisecond (so "how late" is accurate to 1 ms and memory
    // stays bounded on the full day).
    std::vector<std::uint64_t> max_ref_;
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> ref_clock_;
    std::uint64_t adds_ = 0, late_adds_ = 0;
    std::uint64_t late_first_second_ = 0;  // late adds within 1 s after System Event 'Q'
    std::uint64_t late_first_ts_ = 0, late_last_ts_ = 0;
    std::uint64_t late_by_hour_[24] = {};
    std::uint64_t late_delay_[6] = {};  // < 1 ms, < 1 s, < 1 min, < 10 min, < 1 h, >= 1 h
    // Every E: [first in arrival order? no/yes][smallest reference at its level? no/yes]
    std::uint64_t e_order_[2][2] = {};
    // Of the E that were not the smallest reference: was an order with a smaller
    // reference at the level a "late" add?
    std::uint64_t e_not_min_late_ahead_ = 0;

    // Baselines over all E executions (front of queue or not).
    std::uint64_t e_total_ = 0, e_front_ = 0;
    std::uint64_t e_front_odd_lot_ = 0, e_front_mpid_ = 0, e_front_premarket_add_ = 0;
};

}  // namespace tickforge::book
