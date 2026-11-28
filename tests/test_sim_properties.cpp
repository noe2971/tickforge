// Simulator properties (PROGRESS.md, Rule K): no look-ahead, naive fills at
// least as much as queue-aware, and the accounting identity. On random
// hand-built streams, and on the real day's compact file where it exists.

#include <gtest/gtest.h>

#include <iostream>
#include <map>

#include "sim_test_util.hpp"
#include "test_settings.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/stream.hpp"

using namespace tickforge;
using namespace tickforge::sim;
using namespace tickforge::test::simu;
using tickforge::test::Bytes;

namespace {

SimResult run(const SimConfig& cfg, Strategy& s, const std::vector<Bytes>& msgs) {
    Simulator sim(cfg, s);
    feed(sim, msgs);
    sim.finish();
    return sim.result();
}

// Cash and inventory rebuilt from the fills alone must match the simulator's.
void check_accounting(const SimResult& r, const FeeSchedule& fees) {
    Micros cash = 0;
    std::int64_t inv = 0;
    for (const FillRecord& f : r.fills) {
        const auto& x = f.report;
        const Micros notional = static_cast<Micros>(x.price) * x.qty * kMicrosPerTick;
        const Micros fee = x.liquidity == Liquidity::maker ? -fees.maker_rebate_per_share * static_cast<Micros>(x.qty)
                                                           : fees.taker_fee_per_share * static_cast<Micros>(x.qty);
        EXPECT_EQ(x.fee, fee);
        cash += (x.side == Side::buy ? -notional : notional) - fee;
        inv += x.side == Side::buy ? static_cast<std::int64_t>(x.qty) : -static_cast<std::int64_t>(x.qty);
    }
    EXPECT_EQ(cash, r.final_cash);
    EXPECT_EQ(inv, r.final_inventory);
    EXPECT_EQ(r.stats.ahead_underflow, 0u);
    EXPECT_EQ(r.stats.lookahead_violations, 0u);
}

// Decisions made at or before `until`.
std::vector<Decision> upto(const std::vector<Decision>& d, std::uint64_t until) {
    std::vector<Decision> out;
    for (const Decision& x : d)
        if (x.ts <= until) out.push_back(x);
    return out;
}

struct PerOrder {
    std::uint64_t filled = 0;
    std::uint64_t first = kNever;
};
std::map<ShadowId, PerOrder> per_order(const SimResult& r) {
    std::map<ShadowId, PerOrder> m;
    for (const FillRecord& f : r.fills) {
        PerOrder& p = m[f.report.id];
        p.filled += f.report.qty;
        p.first = std::min(p.first, f.report.exchange_ts);
    }
    return m;
}

// Naive >= queue-aware for every order of a fill-independent strategy.
// Returns (naive shares, queue-aware shares).
std::pair<std::uint64_t, std::uint64_t> compare_models(const SimResult& naive, const SimResult& queue) {
    EXPECT_EQ(naive.decisions, queue.decisions) << "the strategy must not depend on fills";
    const auto n = per_order(naive);
    const auto q = per_order(queue);
    for (const auto& [id, qa] : q) {
        const auto it = n.find(id);
        if (it == n.end()) {
            ADD_FAILURE() << "order " << id << " filled " << qa.filled << " queue-aware, 0 naive";
            continue;
        }
        EXPECT_GE(it->second.filled, qa.filled) << "order " << id;
        EXPECT_LE(it->second.first, qa.first) << "order " << id;
    }
    return {naive.stats.filled_shares, queue.stats.filled_shares};
}

}  // namespace

// ---- Look-ahead ------------------------------------------------------------------

TEST(SimLookAhead, DecisionsUpToTDoNotChangeWhenLaterMessagesChange) {
    int later_differs = 0, runs = 0;
    const std::uint64_t seeds = tickforge::test::full_or_short(12, 4, "look-ahead random streams");
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        StreamGen g(seed);
        std::vector<Bytes> base = prologue();
        g.generate(base, 3000);
        const std::uint64_t T = g.time();
        g.advance_time(1);  // every altered message is stamped after T
        std::vector<Bytes> fut_a = base, fut_b = base;
        StreamGen ga = g, gb = g;
        ga.reseed(seed * 1000 + 1);
        gb.reseed(seed * 1000 + 2);
        ga.generate(fut_a, 3000);
        gb.generate(fut_b, 3000);
        for (std::uint64_t md : {0ULL, 20'000ULL}) {
            for (std::uint64_t ol : {0ULL, 50'000ULL}) {
                SimConfig cfg = config(ol, md);
                JoinBestQuoter s1, s2;
                const SimResult a = run(cfg, s1, fut_a);
                const SimResult b = run(cfg, s2, fut_b);
                ++runs;
                ASSERT_FALSE(upto(a.decisions, T).empty()) << "seed " << seed;
                EXPECT_EQ(upto(a.decisions, T + md), upto(b.decisions, T + md)) << "seed " << seed << " md " << md
                                                                                 << " ol " << ol;
                if (a.decisions != b.decisions) ++later_differs;
                EXPECT_EQ(a.stats.lookahead_violations, 0u);
                EXPECT_EQ(b.stats.lookahead_violations, 0u);
                check_accounting(a, cfg.fees);
            }
        }
    }
    // The futures really were different enough to change later decisions.
    EXPECT_EQ(later_differs, runs);
}

TEST(SimLookAhead, EveryMarketEventIsDeliveredExactlyAtItsTimestampPlusLatency) {
    StreamGen g(99);
    std::vector<Bytes> msgs = prologue();
    g.generate(msgs, 2000);
    LambdaStrategy s;
    std::uint64_t bad = 0, n = 0;
    s.market = [&](Context& c, const MarketEvent& e) {
        ++n;
        if (c.now() != e.ts + 30'000) ++bad;
    };
    const SimResult r = run(config(5'000, 30'000), s, msgs);
    EXPECT_GT(n, 1000u);
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(r.stats.lookahead_violations, 0u);
}

// ---- Naive >= queue-aware ----------------------------------------------------------

TEST(SimModels, NaiveFillsAtLeastAsMuchAndNoLaterOnRandomStreams) {
    std::uint64_t naive_total = 0, queue_total = 0;
    const std::uint64_t seeds = tickforge::test::full_or_short(150, 20, "naive-vs-queue random streams");
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        StreamGen g(seed + 500);
        std::vector<Bytes> msgs = prologue();
        g.generate(msgs, 3000);
        for (std::uint64_t ol : {0ULL, 7'000ULL}) {
            for (bool post : {false, true}) {
                TouchSampler sn(40'000, 25'000, 100, kNever, post), sq(40'000, 25'000, 100, kNever, post);
                const SimResult n = run(config(ol, 0, FillModel::naive_touch), sn, msgs);
                const SimResult q = run(config(ol, 0, FillModel::queue_aware), sq, msgs);
                const auto [a, b] = compare_models(n, q);
                naive_total += a;
                queue_total += b;
                check_accounting(n, SimConfig{}.fees);
                check_accounting(q, SimConfig{}.fees);
            }
        }
        // Orders at random prices around the touch (inside the spread, at it,
        // behind it), placed and cancelled on a fixed schedule.
        SplitMix64 rng{seed};
        std::vector<ScheduledAction> plan;
        for (int i = 0; i < 40; ++i) {
            const std::uint64_t t = 1'000'000 + rng.below(g.time() - 1'000'000);
            const Side side = rng.below(2) ? Side::buy : Side::sell;
            const std::uint32_t p = static_cast<std::uint32_t>(100'000 + (static_cast<int>(rng.below(9)) - 4) * 100);
            plan.push_back(place_at(t, side, p, 1 + static_cast<Qty>(rng.below(300))));
            plan.push_back(cancel_at(t + rng.below(200'000), plan.size() - 1));
        }
        ScheduledOrders pn(plan), pq(plan);
        const SimResult n = run(config(3'000, 0, FillModel::naive_touch), pn, msgs);
        const SimResult q = run(config(3'000, 0, FillModel::queue_aware), pq, msgs);
        const auto [a, b] = compare_models(n, q);
        naive_total += a;
        queue_total += b;
        check_accounting(q, SimConfig{}.fees);
    }
    std::cout << "random streams: naive filled " << naive_total << " shares, queue-aware " << queue_total << "\n";
    EXPECT_GT(queue_total, 0u);
    EXPECT_GT(naive_total, queue_total);
}

// ---- On the real day (data/ only; skipped when the file is not there) ------------

namespace {

constexpr std::uint64_t kHour = 3'600'000'000'000ULL;
constexpr std::uint64_t kMinute = 60'000'000'000ULL;

SimConfig day_config(const std::string& sym, FillModel model, std::uint64_t ol, std::uint64_t md) {
    SimConfig c;
    c.symbol = sym;
    c.model = model;
    c.order_latency_ns = ol;
    c.market_data_latency_ns = md;
    c.start_ns = 9 * kHour + 35 * kMinute;
    c.end_ns = 15 * kHour + 55 * kMinute;
    return c;
}

// Passes everything through, except that after time T the chosen stock's
// messages are altered: every third is dropped, adds move 5 cents and double.
struct FutureMutator : itch::NullHandler {
    Simulator& sim;
    std::string symbol;
    std::uint64_t T;
    std::uint16_t loc = 0;
    std::uint64_t n = 0;
    FutureMutator(Simulator& s, std::string sym, std::uint64_t t) : sim(s), symbol(std::move(sym)), T(t) {}

    static void alter(itch::AddOrder& m) {
        m.price += 500;
        m.shares *= 2;
    }
    static void alter(itch::AddOrderMpid& m) {
        m.price += 500;
        m.shares *= 2;
    }
    template <typename M>
    static void alter(M&) {}

    void on(const itch::StockDirectory& m) {
        if (m.stock.view() == symbol) loc = m.header.stock_locate;
        sim.on(m);
    }
    template <typename M>
    void on(const M& m) {
        if (loc != 0 && m.header.stock_locate == loc && m.header.timestamp_ns > T) {
            if (++n % 3 == 0) return;
            M x = m;
            alter(x);
            sim.on(x);
            return;
        }
        sim.on(m);
    }
};

}  // namespace

TEST(SimRealDay, LookAheadOnColbAfterNoon) {
    const std::string path = data_file(kTfc);
    if (path.empty()) GTEST_SKIP() << "needs data/" << kTfc << " (not in git or CI; see data/README.md)";
    const std::uint64_t T = 12 * kHour;
    const SimConfig cfg = day_config("COLB", FillModel::queue_aware, 100'000, 50'000);
    JoinBestQuoter s1, s2;
    const SimResult a = run_file(path, cfg, s1);
    SimResult b;
    {
        auto sim = std::make_unique<Simulator>(cfg, s2);
        FutureMutator mut(*sim, "COLB", T);
        itch::CompactReader src(path);
        itch::parse_stream(src, mut);
        sim->finish();
        b = sim->result();
    }
    ASSERT_TRUE(a.symbol_found);
    const auto da = upto(a.decisions, T + cfg.market_data_latency_ns);
    const auto db = upto(b.decisions, T + cfg.market_data_latency_ns);
    EXPECT_GT(da.size(), 100u);
    EXPECT_EQ(da, db);
    EXPECT_NE(a.decisions, b.decisions) << "altering the afternoon must change afternoon decisions";
    EXPECT_EQ(a.stats.lookahead_violations, 0u);
    EXPECT_EQ(b.stats.lookahead_violations, 0u);
    check_accounting(a, cfg.fees);
}

TEST(SimRealDay, NaiveFillsAtLeastAsMuchAsQueueAwareOnColbAndMsft) {
    const std::string path = data_file(kTfc);
    if (path.empty()) GTEST_SKIP() << "needs data/" << kTfc << " (not in git or CI; see data/README.md)";
    for (const char* sym : {"COLB", "MSFT"}) {
        TouchSampler sn(10'000'000'000ULL, 5'000'000'000ULL, 100), sq(10'000'000'000ULL, 5'000'000'000ULL, 100);
        const SimResult n = run_file(path, day_config(sym, FillModel::naive_touch, 100'000, 0), sn);
        const SimResult q = run_file(path, day_config(sym, FillModel::queue_aware, 100'000, 0), sq);
        ASSERT_TRUE(q.symbol_found) << sym;
        const auto [a, b] = compare_models(n, q);
        std::cout << sym << ": " << q.stats.orders_sent << " orders; naive filled " << a << " shares, queue-aware " << b
                  << "; queue anomalies exec " << q.stats.anomaly_exec_with_ahead << ", cross-add "
                  << q.stats.anomaly_cross_add_with_ahead << "\n";
        EXPECT_GT(n.stats.orders_sent, 1000u);
        EXPECT_GT(a, b) << sym;
        check_accounting(n, SimConfig{}.fees);
        check_accounting(q, SimConfig{}.fees);
    }
}
