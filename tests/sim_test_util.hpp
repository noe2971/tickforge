#pragma once
// Helpers for the simulator tests: a strategy made of lambdas, a recording
// wrapper, and a generator of random but always-consistent ITCH streams for
// one stock (used by the look-ahead and naive-vs-queue property tests).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "itch_book_builders.hpp"
#include "tickforge/sim/simulator.hpp"
#include "tickforge/sim/strategies.hpp"

namespace tickforge::test::simu {

using namespace tickforge::sim;
using itchmsg::feed;

inline constexpr std::uint16_t kLoc = 7;
inline const std::string kSym = "ABC";

// $ -> ITCH Price(4) ticks.
constexpr std::uint32_t px(double dollars) { return static_cast<std::uint32_t>(dollars * 10000.0 + 0.5); }

inline std::vector<Bytes> prologue() {
    return {itchmsg::directory(kLoc, 1, kSym), itchmsg::trading_action(kLoc, 2, kSym, 'T')};
}

inline SimConfig config(std::uint64_t order_latency = 0, std::uint64_t md_latency = 0,
                        FillModel model = FillModel::queue_aware) {
    SimConfig c;
    c.symbol = kSym;
    c.model = model;
    c.order_latency_ns = order_latency;
    c.market_data_latency_ns = md_latency;
    c.pnl_sample_interval_ns = 0;  // no periodic samples unless a test asks
    return c;
}

// Feeds messages one at a time into an existing simulator.
inline void feed_one(Simulator& sim, const Bytes& m) { itch::dispatch(m.b.data(), m.b.size(), sim); }

struct LambdaStrategy : Strategy {
    std::function<void(Context&)> start;
    std::function<void(Context&, const MarketEvent&)> market;
    std::function<void(Context&, const OrderUpdate&)> order;
    std::function<void(Context&, const FillReport&)> fill;
    std::function<void(Context&, std::uint64_t)> timer;
    std::function<void(Context&)> end;
    void on_start(Context& c) override { if (start) start(c); }
    void on_market(Context& c, const MarketEvent& e) override { if (market) market(c, e); }
    void on_order(Context& c, const OrderUpdate& u) override { if (order) order(c, u); }
    void on_fill(Context& c, const FillReport& f) override { if (fill) fill(c, f); }
    void on_timer(Context& c, std::uint64_t id) override { if (timer) timer(c, id); }
    void on_end(Context& c) override { if (end) end(c); }
};

// ScheduledOrders that also records what it is told.
struct RecordingSchedule : ScheduledOrders {
    using ScheduledOrders::ScheduledOrders;
    std::vector<OrderUpdate> updates;
    std::vector<FillReport> fills;
    void on_order(Context&, const OrderUpdate& u) override { updates.push_back(u); }
    void on_fill(Context&, const FillReport& f) override { fills.push_back(f); }
};

inline ScheduledAction place_at(std::uint64_t t, Side side, std::uint32_t price, Qty qty, OrderFlags f = {}) {
    ScheduledAction a;
    a.at_ns = t;
    a.kind = DecisionKind::place;
    a.side = side;
    a.price = price;
    a.qty = qty;
    a.flags = f;
    return a;
}
inline ScheduledAction cancel_at(std::uint64_t t, std::size_t step, Qty shares = 0) {
    ScheduledAction a;
    a.at_ns = t;
    a.kind = DecisionKind::cancel;
    a.target = step;
    a.qty = shares;
    return a;
}
inline ScheduledAction replace_at(std::uint64_t t, std::size_t step, std::uint32_t price, Qty qty) {
    ScheduledAction a;
    a.at_ns = t;
    a.kind = DecisionKind::replace;
    a.target = step;
    a.price = price;
    a.qty = qty;
    return a;
}

// A strategy whose decisions depend only on its book and its clock, never on
// its fills, so it makes identical decisions under both fill models. Every
// `period` it places `qty` at the best bid and at the best ask (as it sees
// them) and cancels both `life` later (a cancel of a filled order is simply
// rejected as too late).
struct TouchSampler : Strategy {
    std::uint64_t period, life, until;
    Qty qty;
    bool post_only;
    std::uint64_t quote_timer = 0;
    std::unordered_map<std::uint64_t, std::vector<ShadowId>> cancel_timers;  // timer id -> orders to cancel
    TouchSampler(std::uint64_t period_ns, std::uint64_t life_ns, Qty q, std::uint64_t until_ns = kNever,
                 bool post = false)
        : period(period_ns), life(life_ns), until(until_ns), qty(q), post_only(post) {}
    void on_start(Context& c) override { quote_timer = c.set_timer(c.now() + period); }
    void on_timer(Context& c, std::uint64_t id) override {
        if (const auto it = cancel_timers.find(id); it != cancel_timers.end()) {
            for (ShadowId oid : it->second) c.cancel(oid);
            cancel_timers.erase(it);
            return;
        }
        if (id != quote_timer) return;
        OrderFlags f;
        f.post_only = post_only;
        std::vector<ShadowId> placed;
        if (const auto b = c.best_bid()) placed.push_back(c.place(Side::buy, b->price, qty, f));
        if (const auto a = c.best_ask()) placed.push_back(c.place(Side::sell, a->price, qty, f));
        if (!placed.empty()) cancel_timers[c.set_timer(c.now() + life)] = placed;
        if (c.now() + period <= until) quote_timer = c.set_timer(c.now() + period);
    }
};

// ---- Random consistent stream for one stock ----------------------------------

// SplitMix64: a tiny PRNG fully specified here, so a seed means the same
// stream with any compiler (std::uniform_int_distribution is not portable).
struct SplitMix64 {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return next() % n; }
};

class StreamGen {
public:
    explicit StreamGen(std::uint64_t seed, std::uint64_t t0 = 1'000'000) : rng_{seed}, t_(t0) {}

    void reseed(std::uint64_t seed) { rng_ = SplitMix64{seed}; }
    std::uint64_t time() const noexcept { return t_; }
    void advance_time(std::uint64_t dt) noexcept { t_ += dt; }

    // Appends n messages; every one is valid against the book built so far.
    void generate(std::vector<Bytes>& out, std::size_t n) {
        using namespace itchmsg;
        for (std::size_t i = 0; i < n; ++i) {
            if (rng_.below(5) != 0) t_ += 1 + rng_.below(20'000);  // sometimes the same timestamp
            const std::uint64_t r = rng_.below(100);
            if (r < 38 || live_.size() < 4) {
                add_order(out, false);
            } else if (r < 40) {
                add_order(out, true);  // late add: an older reference number
            } else if (r < 60) {
                execute_front(out);
            } else if (r < 70) {
                auto& o = pick();
                if (o.shares > 1) {
                    const std::uint32_t k = 1 + static_cast<std::uint32_t>(rng_.below(o.shares - 1));
                    out.push_back(cancel(kLoc, t_, o.ref, k));
                    o.shares -= k;
                }
            } else if (r < 85) {
                const std::size_t k = rng_.below(live_.size());
                out.push_back(del(kLoc, t_, live_[k].ref));
                live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(k));
            } else if (r < 93) {
                const std::size_t k = rng_.below(live_.size());
                Live o = live_[k];
                live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(k));
                const std::uint32_t p = price_for(o.side);
                if (p == 0) {
                    out.push_back(del(kLoc, t_, o.ref));
                    continue;
                }
                const std::uint64_t nref = next_ref_++;
                const std::uint32_t sh = 1 + static_cast<std::uint32_t>(rng_.below(500));
                out.push_back(replace(kLoc, t_, o.ref, nref, sh, p));
                live_.push_back({nref, o.side, p, sh});
            } else if (r < 97) {
                mid_ += rng_.below(2) == 0 ? 100 : -100;
                if (mid_ < 50'000) mid_ = 50'000;
            } else {
                out.push_back(trade(kLoc, t_, rng_.below(2) ? 'B' : 'S', 100, kSym, static_cast<std::uint32_t>(mid_)));
            }
        }
    }

private:
    struct Live {
        std::uint64_t ref;
        char side;
        std::uint32_t price;
        std::uint32_t shares;
    };

    Live& pick() { return live_[rng_.below(live_.size())]; }

    std::uint32_t best(char side) const {
        std::uint32_t b = 0;
        for (const Live& o : live_)
            if (o.side == side && (b == 0 || (side == 'B' ? o.price > b : o.price < b))) b = o.price;
        return b;
    }

    // A price on `side` near the mid that does not cross the other side; 0 if none.
    std::uint32_t price_for(char side) {
        const std::int64_t k = static_cast<std::int64_t>(rng_.below(4)) * 100;
        std::int64_t p = side == 'B' ? mid_ - 100 - k : mid_ + 100 + k;
        const std::uint32_t opp = best(side == 'B' ? 'S' : 'B');
        if (opp != 0) {
            if (side == 'B' && p >= opp) p = static_cast<std::int64_t>(opp) - 100;
            if (side == 'S' && p <= opp) p = static_cast<std::int64_t>(opp) + 100;
        }
        return p > 0 ? static_cast<std::uint32_t>(p) : 0;
    }

    void add_order(std::vector<Bytes>& out, bool late) {
        const char side = rng_.below(2) ? 'B' : 'S';
        const std::uint32_t p = price_for(side);
        if (p == 0) return;
        const std::uint64_t ref = late && late_ref_ > 1 ? --late_ref_ : next_ref_++;
        const std::uint32_t sh = 1 + static_cast<std::uint32_t>(rng_.below(400));
        out.push_back(itchmsg::add(kLoc, t_, ref, side, sh, kSym, p));
        live_.push_back({ref, side, p, sh});
    }

    // Executes the order with the smallest reference at the best price of a
    // side (reference-number priority, as Nasdaq does).
    void execute_front(std::vector<Bytes>& out) {
        const char side = rng_.below(2) ? 'B' : 'S';
        const std::uint32_t b = best(side);
        if (b == 0) return;
        std::size_t k = live_.size();
        for (std::size_t i = 0; i < live_.size(); ++i)
            if (live_[i].side == side && live_[i].price == b && (k == live_.size() || live_[i].ref < live_[k].ref)) k = i;
        Live& o = live_[k];
        const std::uint32_t sh = rng_.below(3) == 0 ? o.shares : 1 + static_cast<std::uint32_t>(rng_.below(o.shares));
        out.push_back(itchmsg::executed(kLoc, t_, o.ref, sh));
        o.shares -= sh;
        if (o.shares == 0) live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(k));
    }

    SplitMix64 rng_;
    std::uint64_t t_;
    std::int64_t mid_ = 100'000;  // $10.00
    std::uint64_t next_ref_ = 1'000'000;
    std::uint64_t late_ref_ = 1'000'000;
    std::vector<Live> live_;
};

// Path of a file in data/, or "" if it is not there.
inline std::string data_file(const std::string& name) {
    std::filesystem::path dir = TICKFORGE_SOURCE_DIR "/data";
    if (const char* env = std::getenv("TICKFORGE_DATA_DIR")) dir = env;
    const auto p = dir / name;
    return std::filesystem::exists(p) ? p.string() : std::string();
}

inline constexpr const char* kTfc = "01302019_AAPL_MSFT_INTC_COLB.tfc";

}  // namespace tickforge::test::simu
