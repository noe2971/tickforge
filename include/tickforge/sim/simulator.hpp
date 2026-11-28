#pragma once
// Simulator: replays one stock from a real Nasdaq ITCH file and lets a
// strategy trade in it with shadow orders (PROGRESS.md, Step 3, Rules G-K).
//
// How time works. Real messages are processed one at a time, in file order.
// Everything else waits in an event queue keyed by (time, class, sequence):
//
//   class 0  a shadow action reaching the exchange (place / cancel / replace)
//   class 1  a measurement (markout, P&L sample, end of run)
//   class 2  a delivery to the strategy (a real message, a fill, an ack)
//   class 3  a strategy timer (and the start)
//
// Before a real message timestamped t is applied, every queued event with
// time < t runs, so an event at exactly t runs *after* the real messages at t
// ("real messages win ties"). A delivery to the strategy is queued at
// exchange time + market-data latency; an action the strategy decides at
// strategy time tau is queued at tau + order latency. The strategy only ever
// reads its own book, built from what was delivered to it, so it cannot see a
// message before its delivery time.
//
// Queue model (Rule G). Every real order's priority is its order reference
// number; a shadow order arriving at the exchange gets key K = the largest
// reference that stock has published so far. Real orders with reference <= K
// at the same side and price are ahead of it; `ahead` counts their shares.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "tickforge/book/fast_book.hpp"
#include "tickforge/itch/decode.hpp"
#include "tickforge/itch/messages.hpp"
#include "tickforge/sim/strategy.hpp"
#include "tickforge/sim/types.hpp"

namespace tickforge::sim {

class Simulator : public itch::NullHandler {
public:
    using itch::NullHandler::on;

    Simulator(SimConfig cfg, Strategy& strategy);
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    // ITCH handler interface (itch/decode.hpp).
    void on(const itch::StockDirectory& m);
    void on(const itch::StockTradingAction& m);
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
        advance(m.header.timestamp_ns);
    }

    // Call once after the last message: runs the remaining events up to the
    // last timestamp and ends the run if the end time was not reached.
    void finish();

    const SimResult& result() const noexcept { return result_; }
    const SimConfig& config() const noexcept { return cfg_; }

    // Exchange-side state, for tests and analysis only. A strategy never gets
    // these (it only has Context).
    const book::FastBook& exchange_book() const noexcept { return xbook_; }
    std::optional<std::uint64_t> ahead_of(ShadowId id) const;  // shares ahead in the queue, if resting
    OrderStatus exchange_status(ShadowId id) const;
    Qty exchange_open(ShadowId id) const;

private:
    class Ctx;
    friend class Ctx;

    struct Shadow {
        ShadowId id = 0;
        Side side = Side::buy;
        Price price = 0;
        Qty qty = 0;
        Qty open = 0;
        Qty filled = 0;
        OrderStatus status = OrderStatus::pending;
        OrderFlags flags;
        std::uint64_t key = 0;    // priority key (Rule G)
        std::uint64_t seq = 0;    // arrival order among shadow orders
        std::uint64_t ahead = 0;  // real shares ahead of it at its price
    };

    enum class EvKind : std::uint8_t { action, markout, sample, end, timer, start };
    struct Event {
        std::uint64_t time;
        std::uint8_t cls;
        std::uint64_t seq;
        EvKind kind;
        std::uint64_t a;  // action index / fill index / timer id
        std::uint32_t b;  // markout horizon
    };
    struct EventLater {
        bool operator()(const Event& x, const Event& y) const noexcept {
            if (x.time != y.time) return x.time > y.time;
            if (x.cls != y.cls) return x.cls > y.cls;
            return x.seq > y.seq;
        }
    };
    struct Action {
        DecisionKind kind;
        ShadowId id;
        ShadowId new_id;
        Qty shares;  // cancel: shares to cancel (0 = all)
    };
    enum class DeliveryKind : std::uint8_t { market, update, fill };
    struct Delivery {
        std::uint64_t time;
        DeliveryKind kind;
        MarketEvent market;
        OrderUpdate update;
        FillReport fill;
    };

    // Event loop.
    void advance(std::uint64_t t);
    void run_events(std::uint64_t limit, bool inclusive);
    void schedule(std::uint64_t time, std::uint8_t cls, EvKind kind, std::uint64_t a = 0, std::uint32_t b = 0);
    void process(const Event& e);
    void process_delivery(const Delivery& d);
    template <typename F>
    void call_strategy(std::uint64_t tau, F&& f);

    // Exchange side.
    bool target(const itch::Header& h) const noexcept { return symbol_found_ && h.stock_locate == locate_; }
    bool trading() const noexcept { return xstate_ == 'T' || xstate_ == 0; }
    std::int64_t mid2_now() noexcept;
    Shadow* shadow(ShadowId id) noexcept;
    void arrive_new(Shadow& s, std::uint64_t t);
    void arrive_cancel(ShadowId id, Qty shares, std::uint64_t t);
    void arrive_replace(ShadowId old_id, ShadowId new_id, std::uint64_t t);
    void reject(Shadow& s, Reject why, std::uint64_t t);
    void take(Shadow& s, std::uint64_t t);
    void fill(Shadow& s, Qty q, Price px, Liquidity liq, char trigger, std::uint64_t t, std::int64_t mid2_before);
    void remove_active(ShadowId id);
    std::vector<ShadowId> candidates(Side side, Price limit_price) const;  // active shadows on `side` that a trade at limit_price reaches, best first
    void real_add(const itch::Header& h, OrderId ref, Side side, Price price, Qty shares, char type);
    void real_exec(const itch::Header& h, OrderId ref, Qty shares, bool priced, Price exec_price, char type, char printable);
    void crossing_add(Side real_side, Price price, Qty shares, char type, std::uint64_t t, std::int64_t mid2);
    void touch(Price trade_price, char type, std::uint64_t t, std::int64_t mid2);
    void reduce_ahead(Side side, Price price, OrderId ref, std::uint64_t shares);
    void late_add(Side side, Price price, OrderId ref, Qty shares);
    void after_real_reduce(OrderId ref);
    void end_run(std::uint64_t t);
    void sample(std::uint64_t t);

    // Deliveries to the strategy.
    void deliver_market(const MarketEvent& m);
    void deliver_update(const OrderUpdate& u);
    void deliver_fill(const FillReport& f);

    SimConfig cfg_;
    Strategy& strategy_;
    std::unique_ptr<Ctx> ctx_;

    // Exchange side (truth).
    bool symbol_found_ = false;
    std::uint16_t locate_ = 0;
    book::FastBook xbook_;
    char xstate_ = 0;
    std::uint64_t max_ref_ = 0;
    std::vector<Shadow> shadows_;   // index = id - 1
    std::vector<ShadowId> active_;  // resting shadow orders
    std::unordered_map<OrderId, Qty> taken_;  // real shares already taken by shadow orders (Rule H)
    std::uint64_t arrival_seq_ = 0;
    Micros cash_ = 0;
    std::int64_t inventory_ = 0;
    std::int64_t last_mid2_ = 0;

    // Event queue.
    std::priority_queue<Event, std::vector<Event>, EventLater> events_;
    std::deque<Delivery> deliveries_;
    std::vector<Action> actions_;
    std::uint64_t seq_ = 0;
    std::uint64_t last_ts_ = 0;
    std::uint64_t timer_seq_ = 0;

    // Strategy side (what it has been told).
    book::FastBook vbook_;
    char vstate_ = 0;
    std::vector<KnownOrder> known_;  // index = id - 1
    std::int64_t known_position_ = 0;
    Micros known_cash_ = 0;
    std::uint64_t tau_ = 0;          // strategy time of the current callback
    std::uint64_t latest_seen_ts_ = 0;
    bool seen_any_ = false;
    bool started_ = false;
    bool ended_ = false;

    SimResult result_;
};

// Replays a .tfc compact file, a .gz day or a raw ITCH file through a
// Simulator and returns its result.
SimResult run_file(const std::string& path, const SimConfig& cfg, Strategy& strategy);

}  // namespace tickforge::sim
