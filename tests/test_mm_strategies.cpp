// Step 4 strategies (PROGRESS.md, Rules N and O): the symmetric inside quoter
// and the Avellaneda-Stoikov market maker. Prices, sizes, the inventory limit,
// the formula, the estimators, and no look-ahead.

#include <gtest/gtest.h>

#include <cmath>

#include "sim_test_util.hpp"
#include "test_settings.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/stream.hpp"
#include "tickforge/sim/mm_strategies.hpp"

using namespace tickforge;
using namespace tickforge::sim;
using namespace tickforge::test::simu;
using namespace tickforge::test::itchmsg;
using tickforge::test::Bytes;

namespace {

constexpr std::uint64_t kSec = 1'000'000'000ULL;
constexpr std::uint64_t kHour = 3'600 * kSec;
constexpr std::uint64_t kMinute = 60 * kSec;

std::vector<Decision> of_side(const std::vector<Decision>& d, Side s) {
    std::vector<Decision> out;
    for (const Decision& x : d)
        if (x.side == s && x.kind != DecisionKind::cancel) out.push_back(x);
    return out;
}

std::vector<Decision> upto(const std::vector<Decision>& d, std::uint64_t until) {
    std::vector<Decision> out;
    for (const Decision& x : d)
        if (x.ts <= until) out.push_back(x);
    return out;
}

void check_accounting(const SimResult& r) {
    Micros cash = 0;
    std::int64_t inv = 0;
    for (const FillRecord& f : r.fills) {
        const auto& x = f.report;
        const Micros notional = static_cast<Micros>(x.price) * x.qty * kMicrosPerTick;
        cash += (x.side == Side::buy ? -notional : notional) - x.fee;
        inv += x.side == Side::buy ? static_cast<std::int64_t>(x.qty) : -static_cast<std::int64_t>(x.qty);
    }
    EXPECT_EQ(cash, r.final_cash);
    EXPECT_EQ(inv, r.final_inventory);
    EXPECT_EQ(r.stats.ahead_underflow, 0u);
    EXPECT_EQ(r.stats.lookahead_violations, 0u);
}

SimResult run(const SimConfig& cfg, Strategy& s, const std::vector<Bytes>& msgs) {
    Simulator sim(cfg, s);
    feed(sim, msgs);
    sim.finish();
    return sim.result();
}

}  // namespace

// ---- Simulator: more markout horizons -------------------------------------------

TEST(SimMarkoutHorizons, SevenHorizonsIncludingZeroAreMeasured) {
    RecordingSchedule strat({place_at(1 * kSec, Side::buy, px(10.01), 100)});
    SimConfig cfg = config();
    cfg.markout_horizons_ns = {0, 1'000'000, 10'000'000, 100'000'000, kSec, 10 * kSec, 60 * kSec};
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, px(10.00)));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, px(10.02)));          // mid 10.01
    m.push_back(add(kLoc, 2 * kSec, 3, 'S', 100, kSym, px(10.01)));    // fills us 100 @ 10.01
    m.push_back(del(kLoc, 2 * kSec, 1));                                // same timestamp: no bid for a moment
    m.push_back(add(kLoc, 2 * kSec, 4, 'B', 100, kSym, px(9.99)));     // mid (9.99 + 10.01) / 2 = 10.00
    m.push_back(add(kLoc, 2 * kSec + 5'000'000, 5, 'B', 100, kSym, px(10.00)));  // 5 ms: mid 10.005
    m.push_back(del(kLoc, 2 * kSec + 50'000'000, 3));                   // 50 ms: ask 10.02, mid 10.01
    m.push_back(add(kLoc, 5 * kSec, 6, 'S', 100, kSym, px(10.01)));    // 5 s: mid 10.005
    m.push_back(add(kLoc, 100 * kSec, 7, 'S', 100, kSym, px(10.05)));
    const SimResult r = run(cfg, strat, m);
    ASSERT_EQ(r.fills.size(), 1u);
    const FillRecord& f = r.fills[0];
    for (std::size_t h = 0; h < 7; ++h) ASSERT_TRUE(f.markout_ok[h]) << h;
    EXPECT_FALSE(f.markout_ok[7]);
    EXPECT_NEAR(f.markout(0), -0.01, 1e-12);   // 0: after every message at 2 s, mid 10.00
    EXPECT_NEAR(f.markout(1), -0.01, 1e-12);   // 1 ms
    EXPECT_NEAR(f.markout(2), -0.005, 1e-12);  // 10 ms: 10.005
    EXPECT_NEAR(f.markout(3), 0.0, 1e-12);     // 100 ms: 10.01
    EXPECT_NEAR(f.markout(4), 0.0, 1e-12);     // 1 s
    EXPECT_NEAR(f.markout(5), -0.005, 1e-12);  // 10 s: 10.005
    EXPECT_NEAR(f.markout(6), -0.005, 1e-12);  // 60 s
}

TEST(SimMarkoutHorizons, MoreThanEightHorizonsIsAnError) {
    RecordingSchedule strat(std::vector<ScheduledAction>{});
    SimConfig cfg = config();
    cfg.markout_horizons_ns.assign(kMaxMarkoutHorizons + 1, kSec);
    EXPECT_THROW({ Simulator sim(cfg, strat); }, std::invalid_argument);
}

// ---- Symmetric inside quoter (Rule N) ---------------------------------------------

TEST(SymmetricQuoter, PricesOneTickInsideOnlyWhenTheSpreadIsAtLeastThreeTicks) {
    using T = SymmetricInsideQuoter;
    using PP = std::pair<Price, Price>;
    auto p = [](double b, double a) {
        const auto t = T::prices(px(b), px(a));
        return PP(*t.bid, *t.ask);
    };
    EXPECT_EQ(p(10.00, 10.05), PP(px(10.01), px(10.04)));
    EXPECT_EQ(p(10.00, 10.03), PP(px(10.01), px(10.02)));
    EXPECT_EQ(p(10.00, 10.02), PP(px(10.00), px(10.02)));  // 2 ticks: join
    EXPECT_EQ(p(10.00, 10.01), PP(px(10.00), px(10.01)));  // 1 tick: join
}

TEST(SymmetricQuoter, QuotesInsideFromQuoteStartAndFollowsTheBest) {
    SymmetricInsideQuoter s(QuoterParams{100, 500, 1000});
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 500, kSym, px(10.00)));
    m.push_back(add(kLoc, 20, 2, 'S', 500, kSym, px(10.05)));
    m.push_back(add(kLoc, 2000, 3, 'B', 100, kSym, px(10.02)));  // best bid 10.02
    m.push_back(add(kLoc, 3000, 4, 'B', 100, kSym, px(9.90)));
    const SimResult r = run(config(), s, m);
    ASSERT_EQ(r.decisions.size(), 3u);
    EXPECT_EQ(r.decisions[0].kind, DecisionKind::place);
    EXPECT_EQ(r.decisions[0].ts, 1000u) << "no order before quote_start";
    EXPECT_EQ(r.decisions[0].side, Side::buy);
    EXPECT_EQ(r.decisions[0].price, px(10.01));
    EXPECT_TRUE(r.decisions[0].flags.post_only);
    EXPECT_EQ(r.decisions[1].side, Side::sell);
    EXPECT_EQ(r.decisions[1].price, px(10.04));
    EXPECT_EQ(r.decisions[2].kind, DecisionKind::replace);
    EXPECT_EQ(r.decisions[2].ts, 2000u);
    EXPECT_EQ(r.decisions[2].price, px(10.03));
    EXPECT_EQ(s.quote_stats().quotes, 3u);
    EXPECT_EQ(s.quote_stats().improved, 3u);
}

TEST(SymmetricQuoter, SizesAreClippedToTheInventoryLimitAndBuyingStopsThere) {
    SymmetricInsideQuoter s(QuoterParams{100, 150, 0});
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 500, kSym, px(10.00)));
    m.push_back(add(kLoc, 20, 2, 'S', 500, kSym, px(10.05)));      // we bid 10.01, offer 10.04
    m.push_back(add(kLoc, 1000, 3, 'S', 300, kSym, px(10.01)));    // crosses our bid: we buy 100
    m.push_back(del(kLoc, 2000, 3));                                // back to 10.00 / 10.05: we bid 10.01 again
    m.push_back(add(kLoc, 3000, 4, 'S', 300, kSym, px(10.01)));    // we buy 50 more: position 150
    m.push_back(del(kLoc, 4000, 4));
    m.push_back(add(kLoc, 5000, 5, 'B', 10, kSym, px(9.90)));
    const SimResult r = run(config(), s, m);
    std::vector<Qty> buy_sizes;
    for (const Decision& d : of_side(r.decisions, Side::buy)) buy_sizes.push_back(d.qty);
    ASSERT_GE(buy_sizes.size(), 2u);
    EXPECT_EQ(buy_sizes[0], 100u);
    EXPECT_EQ(buy_sizes.back(), 50u) << "room left = 150 - 100";
    EXPECT_EQ(r.final_inventory, 150);
    EXPECT_EQ(r.stats.max_inventory, 150);
    EXPECT_EQ(r.stats.bought_shares, 150u);
    for (const Decision& d : r.decisions) {
        if (d.side == Side::buy && d.kind != DecisionKind::cancel) EXPECT_LT(d.ts, 3000u) << "no buying at the limit";
    }
    check_accounting(r);
}

TEST(SymmetricQuoter, CancelsBothSidesWhenTradingHalts) {
    SymmetricInsideQuoter s(QuoterParams{100, 500, 0});
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 500, kSym, px(10.00)));
    m.push_back(add(kLoc, 20, 2, 'S', 500, kSym, px(10.05)));
    m.push_back(trading_action(kLoc, 1000, kSym, 'H'));
    m.push_back(add(kLoc, 2000, 3, 'B', 100, kSym, px(10.02)));
    const SimResult r = run(config(), s, m);
    int cancels = 0;
    for (const Decision& d : r.decisions) {
        if (d.kind == DecisionKind::cancel) {
            ++cancels;
            EXPECT_EQ(d.ts, 1000u);
        } else {
            EXPECT_LT(d.ts, 1000u) << "no new quotes while halted";
        }
    }
    EXPECT_EQ(cancels, 2);
}

// ---- Avellaneda-Stoikov formula (Rule O) ------------------------------------------

TEST(AvellanedaStoikovFormula, FlatInventoryOneTickSpreadJoinsTheBest) {
    const AsQuote q = as_quote(10.005, 0, 0.0, 0.005, 1000.0, 4.56e-5, px(10.00), px(10.01));
    EXPECT_DOUBLE_EQ(q.reservation, 10.005);
    EXPECT_DOUBLE_EQ(q.half_spread, 0.005);  // ln(1 + g m) / g is just below m: the half-tick floor applies
    EXPECT_EQ(q.bid, std::optional<Price>(px(10.00)));
    EXPECT_EQ(q.ask, std::optional<Price>(px(10.01)));
}

TEST(AvellanedaStoikovFormula, ReservationAndSpreadMatchThePaper) {
    const double g = 4.56e-5, var = 1e-4, tau = 22'800, m = 0.005;
    const double risk = g * var * tau;  // 1.03968e-4 $ per share
    const AsQuote q = as_quote(10.005, 500, var, m, tau, g, px(10.00), px(10.01));
    EXPECT_NEAR(q.reservation, 10.005 - 500 * risk, 1e-12);
    EXPECT_NEAR(q.half_spread, 0.5 * risk + std::log1p(g * m) / g, 1e-15);
    // r - d/2 = 9.947966 -> nearest cent at or below = 9.95; the ask, 9.958, is
    // raised to best bid + 1 tick = 10.01 so it does not cross.
    EXPECT_EQ(q.bid, std::optional<Price>(px(9.95)));
    EXPECT_EQ(q.ask, std::optional<Price>(px(10.01)));
    // Short inventory mirrors it.
    const AsQuote s = as_quote(10.005, -500, var, m, tau, g, px(10.00), px(10.01));
    EXPECT_EQ(s.bid, std::optional<Price>(px(10.00)));
    EXPECT_EQ(s.ask, std::optional<Price>(px(10.06)));
}

TEST(AvellanedaStoikovFormula, OneLotAtTheOpenMovesATypicalStockOneTick) {
    // The design calibration of gamma (Rule O): $100 stock, 1.5% daily vol.
    const double var = 1.5 * 1.5 / 23'400.0;
    EXPECT_NEAR(4.56e-5 * 100 * var * 22'800, 0.01, 0.0001);
}

TEST(AvellanedaStoikovFormula, WideSpreadQuotesAtMeanExecutionDistanceAndNoSkewAtTheEnd) {
    const AsQuote q = as_quote(36.45, 0, 0.0, 0.03, 0.0, 4.56e-5, px(36.40), px(36.50));
    EXPECT_NEAR(q.half_spread, 0.03, 1e-6);
    EXPECT_EQ(q.bid, std::optional<Price>(px(36.42)));
    EXPECT_EQ(q.ask, std::optional<Price>(px(36.48)));
    const AsQuote end = as_quote(36.45, 500, 1e-3, 0.03, 0.0, 4.56e-5, px(36.40), px(36.50));
    EXPECT_DOUBLE_EQ(end.reservation, 36.45) << "T - t = 0: inventory no longer skews";
}

TEST(AvellanedaStoikovFormula, BidIsAlwaysBelowAsk) {
    for (int i = 0; i < 2000; ++i) {
        const double mid = 10.0 + 0.0005 * i;
        const AsQuote q = as_quote(mid, (i % 21 - 10) * 50, 1e-6 * (i % 7), 0.0001 * (i % 13), 100.0 * (i % 11),
                                   4.56e-5, px(9.0), px(12.0));
        ASSERT_TRUE(q.bid && q.ask);
        EXPECT_LT(*q.bid, *q.ask) << i;
    }
}

// ---- Avellaneda-Stoikov estimators -------------------------------------------------

namespace {
std::vector<Bytes> estimator_stream() {
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 500, kSym, px(10.00)));
    m.push_back(add(kLoc, 20, 2, 'S', 500, kSym, px(10.02)));                    // mid 10.01
    m.push_back(executed(kLoc, kSec / 2, 2, 10));                                  // at 10.02: 1 cent from mid
    m.push_back(executed_price(kLoc, kSec / 2 + 1, 1, 10, 'N', px(10.00)));       // non-printable: ignored
    m.push_back(executed_price(kLoc, kSec / 2 + 2, 1, 10, 'Y', px(10.00)));       // 1 cent from mid
    m.push_back(trade(kLoc, kSec / 2 + 3, 'B', 100, kSym, px(10.01)));            // hidden trade: ignored
    m.push_back(del(kLoc, 3 * kSec / 2, 2));                                       // one-sided for a moment
    m.push_back(add(kLoc, 3 * kSec / 2, 3, 'S', 500, kSym, px(10.04)));          // mid 10.02
    m.push_back(del(kLoc, 5 * kSec / 2, 3));
    m.push_back(add(kLoc, 5 * kSec / 2, 4, 'S', 500, kSym, px(10.02)));          // mid 10.01
    m.push_back(add(kLoc, 9 * kSec / 2, 5, 'B', 1, kSym, px(9.00)));             // after the 4 s sample
    return m;
}
}  // namespace

TEST(AvellanedaStoikovEstimators, VarianceOfOneSecondMidChangesAndMeanExecutionDistance) {
    // Samples at 1, 2, 3, 4 s: mid 10.01, 10.02, 10.01, 10.01 -> changes +1c, -1c, 0.
    AvellanedaStoikov s(QuoterParams{100, 500, kNever}, AsParams{});
    run(config(), s, estimator_stream());
    EXPECT_EQ(s.sigma_changes(), 3u);
    EXPECT_NEAR(s.variance_per_second(), (1e-4 + 1e-4 + 0.0) / 3.0, 1e-15);
    EXPECT_EQ(s.executions(), 2u);
    EXPECT_NEAR(s.mean_exec_distance(), 0.01, 1e-15);

    AsParams w;
    w.sigma_window = 2;  // keeps only the last two changes: -1c, 0
    AvellanedaStoikov s2(QuoterParams{100, 500, kNever}, w);
    run(config(), s2, estimator_stream());
    EXPECT_EQ(s2.sigma_changes(), 2u);
    EXPECT_NEAR(s2.variance_per_second(), 1e-4 / 2.0, 1e-15);
}

TEST(AvellanedaStoikovEstimators, NoQuotesUntilEnoughHistory) {
    AsParams a;
    a.min_sigma_changes = 4;  // the stream only gives 3
    a.min_executions = 1;
    a.horizon_end_ns = 100 * kSec;
    AvellanedaStoikov s(QuoterParams{100, 500, 0}, a);
    const SimResult r = run(config(), s, estimator_stream());
    EXPECT_FALSE(s.ready());
    EXPECT_TRUE(r.decisions.empty());

    a.min_sigma_changes = 3;
    AvellanedaStoikov s2(QuoterParams{100, 500, 0}, a);
    const SimResult r2 = run(config(), s2, estimator_stream());
    EXPECT_TRUE(s2.ready());
    ASSERT_FALSE(r2.decisions.empty());
    EXPECT_EQ(r2.decisions[0].ts, 4 * kSec) << "ready at the third change (the 4 s sample)";
}

TEST(AvellanedaStoikovStrategy, BuyingLowersTheQuotes) {
    AsParams a;
    a.gamma = 0.01;
    a.min_sigma_changes = 1;
    a.min_executions = 1;
    a.horizon_end_ns = 1000 * kSec;
    a.sample_interval_ns = kSec;
    AvellanedaStoikov s(QuoterParams{100, 500, 0}, a);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 500, kSym, px(10.00)));
    m.push_back(add(kLoc, 20, 2, 'S', 500, kSym, px(10.10)));
    m.push_back(executed(kLoc, kSec / 2, 2, 10));                               // 5 cents from mid 10.05
    m.push_back(del(kLoc, 3 * kSec / 2, 2));
    m.push_back(add(kLoc, 3 * kSec / 2, 3, 'S', 500, kSym, px(10.12)));       // mid 10.06: change +1c
    m.push_back(add(kLoc, 5 * kSec / 2, 4, 'B', 1, kSym, px(9.50)));          // ready after the 2 s sample
    // At 2 s: var 1e-4 $^2/s, 1/k = $0.05, so r = 10.06 and d/2 = 0.0505: we bid
    // 10.01 (inside) and offer 10.11. A real sell at 10.01 hits our bid.
    m.push_back(add(kLoc, 3 * kSec, 5, 'S', 100, kSym, px(10.01)));
    m.push_back(del(kLoc, 4 * kSec, 5));
    m.push_back(add(kLoc, 5 * kSec, 6, 'B', 1, kSym, px(9.50)));
    const SimResult r = run(config(), s, m);
    ASSERT_FALSE(r.fills.empty());
    EXPECT_EQ(r.fills[0].report.side, Side::buy);
    const std::uint64_t t_fill = r.fills[0].report.exchange_ts;
    Price before = 0, after = 0;
    for (const Decision& d : of_side(r.decisions, Side::buy)) {
        if (d.ts < t_fill) before = d.price;
        else if (after == 0) after = d.price;
    }
    ASSERT_GT(before, 0);
    ASSERT_GT(after, 0);
    EXPECT_LT(after, before) << "long inventory moves the reservation price, and the bid, down";
    check_accounting(r);
}

// ---- No look-ahead ---------------------------------------------------------------

TEST(MmStrategiesLookAhead, DecisionsUpToTDoNotChangeWhenLaterMessagesChange) {
    const std::uint64_t seeds = tickforge::test::full_or_short(8, 3, "mm strategies look-ahead random streams");
    int runs = 0, later_differs = 0;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        StreamGen g(seed);
        std::vector<Bytes> base = prologue();
        g.generate(base, 3000);
        const std::uint64_t T = g.time();
        g.advance_time(1);
        std::vector<Bytes> fa = base, fb = base;
        StreamGen ga = g, gb = g;
        ga.reseed(seed * 1000 + 1);
        gb.reseed(seed * 1000 + 2);
        ga.generate(fa, 3000);
        gb.generate(fb, 3000);
        for (std::uint64_t lat : {0ULL, 20'000ULL}) {
            for (int kind = 0; kind < 2; ++kind) {
                AsParams ap;
                ap.sample_interval_ns = 500'000;  // the streams last tens of ms
                ap.min_sigma_changes = 5;
                ap.min_executions = 5;
                ap.horizon_end_ns = 10 * kSec;
                SymmetricInsideQuoter sa(QuoterParams{100, 300, 0}), sb(QuoterParams{100, 300, 0});
                AvellanedaStoikov aa(QuoterParams{100, 300, 0}, ap), ab(QuoterParams{100, 300, 0}, ap);
                Strategy& x = kind == 0 ? static_cast<Strategy&>(sa) : static_cast<Strategy&>(aa);
                Strategy& y = kind == 0 ? static_cast<Strategy&>(sb) : static_cast<Strategy&>(ab);
                const SimConfig cfg = config(lat, lat);
                const SimResult a = run(cfg, x, fa);
                const SimResult b = run(cfg, y, fb);
                ++runs;
                ASSERT_FALSE(upto(a.decisions, T).empty()) << "seed " << seed << " kind " << kind;
                EXPECT_EQ(upto(a.decisions, T + lat), upto(b.decisions, T + lat))
                    << "seed " << seed << " kind " << kind << " latency " << lat;
                if (a.decisions != b.decisions) ++later_differs;
                check_accounting(a);
                check_accounting(b);
            }
        }
    }
    EXPECT_EQ(later_differs, runs);
}

namespace {
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

TEST(MmStrategiesRealDay, LookAheadOnColbAfterNoonWithTheStudyParameters) {
    const std::string path = data_file(kTfc);
    if (path.empty()) GTEST_SKIP() << "needs data/" << kTfc << " (not in git or CI; see data/README.md)";
    const std::uint64_t T = 12 * kHour;
    SimConfig cfg;
    cfg.symbol = "COLB";
    cfg.order_latency_ns = 100'000;
    cfg.market_data_latency_ns = 100'000;
    cfg.start_ns = 9 * kHour + 30 * kMinute;
    cfg.end_ns = 15 * kHour + 55 * kMinute;
    const QuoterParams qp{100, 500, 9 * kHour + 35 * kMinute};
    AsParams ap;
    ap.horizon_end_ns = cfg.end_ns;
    for (int kind = 0; kind < 2; ++kind) {
        SymmetricInsideQuoter sa(qp), sb(qp);
        AvellanedaStoikov aa(qp, ap), ab(qp, ap);
        Strategy& x = kind == 0 ? static_cast<Strategy&>(sa) : static_cast<Strategy&>(aa);
        Strategy& y = kind == 0 ? static_cast<Strategy&>(sb) : static_cast<Strategy&>(ab);
        const SimResult a = run_file(path, cfg, x);
        SimResult b;
        {
            auto sim = std::make_unique<Simulator>(cfg, y);
            FutureMutator mut(*sim, "COLB", T);
            itch::CompactReader src(path);
            itch::parse_stream(src, mut);
            sim->finish();
            b = sim->result();
        }
        ASSERT_TRUE(a.symbol_found);
        const auto da = upto(a.decisions, T + cfg.market_data_latency_ns);
        EXPECT_GT(da.size(), 50u) << "kind " << kind;
        EXPECT_EQ(da, upto(b.decisions, T + cfg.market_data_latency_ns)) << "kind " << kind;
        EXPECT_NE(a.decisions, b.decisions) << "kind " << kind;
        for (const Decision& d : a.decisions) EXPECT_GE(d.ts, qp.quote_start_ns) << "an order before 09:35";
        check_accounting(a);
        check_accounting(b);
    }
}
