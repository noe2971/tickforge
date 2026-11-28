// Simulator on hand-built ITCH streams where the right fill is known in
// advance (PROGRESS.md, Rule K).

#include <gtest/gtest.h>

#include "sim_test_util.hpp"

using namespace tickforge;
using namespace tickforge::sim;
using namespace tickforge::test::simu;
using namespace tickforge::test::itchmsg;
using tickforge::test::Bytes;

namespace {

const std::uint32_t P1000 = px(10.00), P1001 = px(10.01), P1002 = px(10.02), P999 = px(9.99), P998 = px(9.98);

}  // namespace

// ---- Queue position ------------------------------------------------------------

TEST(SimQueue, OrderBehind500SharesFillsOnlyAfter500SharesTrade) {
    RecordingSchedule strat({place_at(100, Side::buy, P1000, 100)});
    Simulator sim(config(), strat);
    std::vector<Bytes> msgs = prologue();
    msgs.push_back(add(kLoc, 10, 1, 'B', 200, kSym, P1000));
    msgs.push_back(add(kLoc, 20, 2, 'B', 300, kSym, P1000));
    msgs.push_back(add(kLoc, 25, 3, 'S', 500, kSym, P1001));
    feed(sim, msgs);
    feed_one(sim, add(kLoc, 150, 4, 'B', 100, kSym, P1000));  // arrives after us: behind
    ASSERT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(500));
    feed_one(sim, executed(kLoc, 200, 1, 200));
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(300));
    feed_one(sim, executed(kLoc, 300, 2, 250));
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(50));
    feed_one(sim, executed(kLoc, 400, 2, 50));
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(0));
    EXPECT_TRUE(sim.result().fills.empty()) << "500 shares traded, all of them ahead of us";
    feed_one(sim, executed(kLoc, 500, 4, 60));  // an order *behind* us trades: we would have been hit first
    ASSERT_EQ(sim.result().fills.size(), 1u);
    EXPECT_EQ(sim.result().fills[0].report.qty, 60u);
    EXPECT_EQ(sim.result().fills[0].report.price, static_cast<Price>(P1000));
    EXPECT_EQ(sim.result().fills[0].report.liquidity, Liquidity::maker);
    EXPECT_EQ(sim.result().fills[0].report.exchange_ts, 500u);
    EXPECT_EQ(sim.result().fills[0].trigger, 'E');
    EXPECT_EQ(sim.exchange_open(1), 40u);  // partial fill
    feed_one(sim, executed(kLoc, 600, 4, 40));
    feed_one(sim, add(kLoc, 700, 5, 'S', 100, kSym, P1002));
    sim.finish();
    const SimResult& r = sim.result();
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.fills[1].report.qty, 40u);
    EXPECT_EQ(r.fills[1].report.open_qty, 0u);
    EXPECT_EQ(sim.exchange_status(1), OrderStatus::filled);
    // Cash: bought 100 x $10.00, earned the $0.0020 maker rebate on 100 shares.
    EXPECT_EQ(r.final_cash, -100 * 100'000 * 100 + 100 * 2000);
    EXPECT_EQ(r.final_inventory, 100);
    EXPECT_EQ(r.stats.rebates, 200'000);
    EXPECT_EQ(r.stats.fees_paid, 0);
    // The strategy was told about both fills.
    ASSERT_EQ(strat.fills.size(), 2u);
    EXPECT_EQ(strat.fills[0].qty + strat.fills[1].qty, 100u);
    EXPECT_EQ(r.stats.anomaly_exec_with_ahead, 0u);
    EXPECT_EQ(r.stats.ahead_underflow, 0u);
}

TEST(SimQueue, CancelsAndDeletesAheadMoveTheOrderUp) {
    RecordingSchedule strat({place_at(100, Side::buy, P1000, 100)});
    Simulator sim(config(), strat);
    std::vector<Bytes> msgs = prologue();
    msgs.push_back(add(kLoc, 10, 1, 'B', 200, kSym, P1000));
    msgs.push_back(add(kLoc, 20, 2, 'B', 300, kSym, P1000));
    msgs.push_back(add(kLoc, 25, 3, 'S', 500, kSym, P1001));
    msgs.push_back(add(kLoc, 150, 4, 'B', 100, kSym, P1000));  // behind
    feed(sim, msgs);
    ASSERT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(500));
    feed_one(sim, cancel(kLoc, 200, 1, 150));
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(350));
    feed_one(sim, del(kLoc, 300, 2));
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(50));
    feed_one(sim, cancel(kLoc, 350, 4, 50));  // a cancel *behind* us changes nothing
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(50));
    feed_one(sim, cancel(kLoc, 400, 1, 50));
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(0));
    EXPECT_TRUE(sim.result().fills.empty());  // nothing traded
    feed_one(sim, executed(kLoc, 500, 4, 30));
    sim.finish();
    ASSERT_EQ(sim.result().fills.size(), 1u);
    EXPECT_EQ(sim.result().fills[0].report.qty, 30u);
}

TEST(SimQueue, ReplacedOrderAheadGoesBehindAndExecutionsAtBetterPricesDoNotFill) {
    RecordingSchedule strat({place_at(100, Side::buy, P1000, 100)});
    Simulator sim(config(), strat);
    std::vector<Bytes> msgs = prologue();
    msgs.push_back(add(kLoc, 10, 1, 'B', 200, kSym, P1000));
    msgs.push_back(add(kLoc, 25, 3, 'S', 500, kSym, P1002));
    feed(sim, msgs);
    feed_one(sim, replace(kLoc, 200, 1, 10, 200, P1000));  // same price, new reference: now behind us
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(0));
    feed_one(sim, add(kLoc, 250, 11, 'B', 100, kSym, P1001));  // a better bid
    feed_one(sim, executed(kLoc, 300, 11, 100));               // trades at $10.01: not our price
    EXPECT_TRUE(sim.result().fills.empty());
    feed_one(sim, executed(kLoc, 400, 10, 70));  // the replaced order, now behind us, trades
    sim.finish();
    ASSERT_EQ(sim.result().fills.size(), 1u);
    EXPECT_EQ(sim.result().fills[0].report.qty, 70u);
}

TEST(SimQueue, LateAddWithOlderReferenceGoesAheadOfTheShadowOrder) {
    // Rule G: priority is reference-number order. A real order published
    // after the shadow arrived, but with a smaller reference, is ahead of it.
    RecordingSchedule strat({place_at(100, Side::buy, P1000, 100)});
    Simulator sim(config(), strat);
    std::vector<Bytes> msgs = prologue();
    msgs.push_back(add(kLoc, 10, 1000, 'B', 200, kSym, P1000));
    msgs.push_back(add(kLoc, 20, 1001, 'S', 500, kSym, P1002));
    feed(sim, msgs);
    feed_one(sim, add(kLoc, 200, 500, 'B', 300, kSym, P1000));  // late add (ref 500 < 1001)
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(500));
    feed_one(sim, add(kLoc, 250, 1002, 'B', 100, kSym, P1000));  // normal add: behind
    EXPECT_EQ(sim.ahead_of(1), std::optional<std::uint64_t>(500));
    feed_one(sim, executed(kLoc, 300, 500, 300));
    feed_one(sim, executed(kLoc, 400, 1000, 200));
    EXPECT_TRUE(sim.result().fills.empty());
    feed_one(sim, executed(kLoc, 500, 1002, 100));
    sim.finish();
    ASSERT_EQ(sim.result().fills.size(), 1u);
    EXPECT_EQ(sim.result().fills[0].report.qty, 100u);
    EXPECT_EQ(sim.result().stats.anomaly_exec_with_ahead, 0u);
}

TEST(SimQueue, PartialCancelKeepsPriorityReplaceLosesIt) {
    // Order 1: 300 shares, first at its level. Order 2 (step 2) replaces
    // order 1's twin at the same price and must queue behind the real order.
    RecordingSchedule strat({place_at(100, Side::buy, P1000, 300), cancel_at(300, 0, 100),
                             place_at(100, Side::sell, P1002, 100), replace_at(300, 2, P1002, 100)});
    Simulator sim(config(), strat);
    std::vector<Bytes> msgs = prologue();
    msgs.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P999));
    msgs.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1001));
    msgs.push_back(add(kLoc, 200, 3, 'B', 100, kSym, P1000));  // behind our buy
    msgs.push_back(add(kLoc, 210, 4, 'S', 100, kSym, P1002));  // behind our sell, ahead of its replacement
    msgs.push_back(executed(kLoc, 400, 3, 100));                // our buy (200 left after the partial cancel) is first
    msgs.push_back(executed(kLoc, 500, 4, 60));                 // ahead of the replacement: no fill
    msgs.push_back(add(kLoc, 600, 5, 'S', 100, kSym, P1002));
    msgs.push_back(executed(kLoc, 700, 5, 30));                 // behind the replacement, but 40 shares of order 4 are still ahead
    feed(sim, msgs);
    sim.finish();
    const SimResult& r = sim.result();
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].report.id, 1u);
    EXPECT_EQ(r.fills[0].report.qty, 100u);
    EXPECT_EQ(r.fills[0].report.open_qty, 100u);  // 300 - 100 cancelled - 100 filled
    // The trade behind the replacement while order 4 was still ahead is
    // exactly the case Rule H counts as an anomaly.
    EXPECT_EQ(r.stats.anomaly_exec_with_ahead, 1u);
}

// ---- Latency --------------------------------------------------------------------

namespace {
// Bid 9.99 / ask 10.01; at 1.005 ms a real bid of 100 arrives at $10.00;
// at 2 ms it trades; at 3 ms another bid of 100 arrives at $10.00 and at 4 ms
// it trades. The strategy decides at 1 ms to bid 100 at $10.00.
std::vector<Bytes> latency_day() {
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P999));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1001));
    m.push_back(add(kLoc, 1'005'000, 3, 'B', 100, kSym, P1000));
    m.push_back(executed(kLoc, 2'000'000, 3, 100));
    m.push_back(add(kLoc, 3'000'000, 4, 'B', 100, kSym, P1000));
    m.push_back(executed(kLoc, 4'000'000, 4, 100));
    m.push_back(add(kLoc, 5'000'000, 5, 'S', 100, kSym, P1002));
    return m;
}
}  // namespace

TEST(SimLatency, TenMicrosecondsPutsTheOrderBehindOneThatArrived5usEarlier) {
    RecordingSchedule strat({place_at(1'000'000, Side::buy, P1000, 100)});
    Simulator sim(config(10'000), strat);  // order latency 10 us: arrives at 1.010 ms
    feed(sim, latency_day());
    sim.finish();
    const SimResult& r = sim.result();
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].report.exchange_ts, 4'000'000u) << "the 2 ms trade went to the order ahead of us";
    ASSERT_FALSE(strat.updates.empty());
    EXPECT_EQ(strat.updates[0].kind, UpdateKind::accepted);
    EXPECT_EQ(strat.updates[0].exchange_ts, 1'010'000u);
}

TEST(SimLatency, ZeroLatencyPutsTheSameOrderAheadAndItFillsFirst) {
    RecordingSchedule strat({place_at(1'000'000, Side::buy, P1000, 100)});
    Simulator sim(config(0), strat);
    feed(sim, latency_day());
    sim.finish();
    const SimResult& r = sim.result();
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].report.exchange_ts, 2'000'000u);
}

TEST(SimLatency, ArrivingAtTheSameNanosecondAsARealOrderQueuesBehindIt) {
    RecordingSchedule strat({place_at(1'000'000, Side::buy, P1000, 100)});
    Simulator sim(config(5'000), strat);  // arrives at exactly 1.005 ms: real messages win ties
    feed(sim, latency_day());
    sim.finish();
    ASSERT_EQ(sim.result().fills.size(), 1u);
    EXPECT_EQ(sim.result().fills[0].report.exchange_ts, 4'000'000u);
}

TEST(SimLatency, CancelArrivingAfterTheFillIsRejectedAsTooLate) {
    RecordingSchedule strat({place_at(1'000'000, Side::buy, P1000, 100), cancel_at(2'000'000, 0)});
    Simulator sim(config(10'000), strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P999));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1001));
    m.push_back(add(kLoc, 1'500'000, 3, 'B', 100, kSym, P1000));  // behind us
    m.push_back(executed(kLoc, 2'005'000, 3, 100));               // fills us while the cancel is in flight
    m.push_back(add(kLoc, 3'000'000, 4, 'S', 100, kSym, P1002));
    feed(sim, m);
    sim.finish();
    ASSERT_EQ(sim.result().fills.size(), 1u);
    EXPECT_EQ(sim.result().stats.rejected[static_cast<std::size_t>(Reject::too_late)], 1u);
    ASSERT_EQ(strat.updates.size(), 2u);
    EXPECT_EQ(strat.updates[1].kind, UpdateKind::cancel_rejected);
    EXPECT_EQ(strat.updates[1].reason, Reject::too_late);
    EXPECT_EQ(strat.updates[1].exchange_ts, 2'010'000u);
}

TEST(SimLatency, StrategySeesMarketDataOnlyUpToItsDecisionTime) {
    // Market-data latency 1 ms: a message stamped t reaches the strategy at
    // t + 1 ms, and at a timer at time T its book holds exactly the messages
    // stamped <= T - 1 ms.
    LambdaStrategy s;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;  // (now, message ts)
    std::vector<std::optional<book::LevelView>> at_timer;
    s.start = [&](Context& c) {
        c.set_timer(2'500'000);
        c.set_timer(3'000'000);
    };
    s.market = [&](Context& c, const MarketEvent& e) { seen.emplace_back(c.now(), e.ts); };
    s.timer = [&](Context& c, std::uint64_t) { at_timer.push_back(c.best_bid()); };
    Simulator sim(config(0, 1'000'000), s);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 1'000'000, 1, 'B', 100, kSym, P999));
    m.push_back(add(kLoc, 1'600'000, 2, 'B', 100, kSym, P1000));  // visible from 2.6 ms
    m.push_back(add(kLoc, 2'000'000, 3, 'B', 100, kSym, P1001));  // visible from 3.0 ms (tie: delivery before timer)
    m.push_back(add(kLoc, 9'000'000, 4, 'S', 100, kSym, P1002));
    feed(sim, m);
    sim.finish();
    for (const auto& [now, ts] : seen) EXPECT_EQ(now, ts + 1'000'000);
    ASSERT_EQ(at_timer.size(), 2u);
    ASSERT_TRUE(at_timer[0].has_value());
    EXPECT_EQ(at_timer[0]->price, static_cast<Price>(P999));   // at 2.5 ms: only the 1.0 ms message
    ASSERT_TRUE(at_timer[1].has_value());
    EXPECT_EQ(at_timer[1]->price, static_cast<Price>(P1001));  // at 3.0 ms: up to the 2.0 ms message
    EXPECT_EQ(sim.result().stats.lookahead_violations, 0u);
}

// ---- Taking, rejections, halts, naive model ---------------------------------------

TEST(SimTaking, CrossingOrderTakesDisplayedLiquidityOnceAndPaysTheTakerFee) {
    OrderFlags ioc;
    ioc.ioc = true;
    RecordingSchedule strat({place_at(100, Side::buy, P1002, 250, ioc), place_at(200, Side::buy, P1002, 100, ioc),
                             place_at(400, Side::buy, P1002, 100, ioc), place_at(600, Side::buy, P1002, 100, ioc)});
    Simulator sim(config(), strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P999));
    m.push_back(add(kLoc, 20, 30, 'S', 100, kSym, P1001));
    m.push_back(add(kLoc, 30, 31, 'S', 200, kSym, P1002));
    m.push_back(add(kLoc, 150, 32, 'B', 1, kSym, P998));   // advances time past the 1st order
    m.push_back(executed(kLoc, 300, 31, 120));             // ref 31 now has 80 left; we used 200 of it
    m.push_back(add(kLoc, 500, 33, 'S', 100, kSym, P1002));
    m.push_back(add(kLoc, 700, 34, 'B', 1, kSym, P998));
    feed(sim, m);
    sim.finish();
    const SimResult& r = sim.result();
    // 1st: 100 @ 10.01 + 150 @ 10.02. 2nd: only 50 of ref 31 left for us.
    // 3rd: nothing left (ref 31 is down to 80 shares, all already used). 4th: 100 of ref 33.
    ASSERT_EQ(r.fills.size(), 4u);
    EXPECT_EQ(r.fills[0].report.qty, 100u);
    EXPECT_EQ(r.fills[0].report.price, static_cast<Price>(P1001));
    EXPECT_EQ(r.fills[1].report.qty, 150u);
    EXPECT_EQ(r.fills[1].report.price, static_cast<Price>(P1002));
    EXPECT_EQ(r.fills[2].report.qty, 50u);
    EXPECT_EQ(r.fills[3].report.qty, 100u);
    for (const auto& f : r.fills) {
        EXPECT_EQ(f.report.liquidity, Liquidity::taker);
        EXPECT_EQ(f.trigger, 'T');
    }
    EXPECT_EQ(r.stats.taker_shares, 400u);
    EXPECT_EQ(r.stats.fees_paid, 400 * 3000);
    const Micros notional = (100LL * P1001 + 300LL * P1002) * 100;
    EXPECT_EQ(r.final_cash, -notional - 400 * 3000);
    // IOC remainders were cancelled: 50 (2nd) and 100 (3rd).
    int cancelled = 0;
    for (const auto& u : strat.updates)
        if (u.kind == UpdateKind::cancelled) cancelled += static_cast<int>(u.cancelled_qty);
    EXPECT_EQ(cancelled, 150);
}

TEST(SimTaking, NonIocRemainderRestsAndPostOnlyAndSelfCrossAreRejected) {
    OrderFlags post;
    post.post_only = true;
    RecordingSchedule strat({place_at(100, Side::buy, P1001, 300),        // takes 100, rests 200 at 10.01
                             place_at(100, Side::buy, P1002, 100, post),  // post-only crossing 10.02: rejected
                             place_at(100, Side::sell, P1001, 100),       // would cross our own bid: rejected
                             place_at(100, Side::sell, P1002, 100, post)});
    Simulator sim(config(), strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P999));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1001));
    m.push_back(add(kLoc, 30, 3, 'S', 100, kSym, P1002));
    m.push_back(add(kLoc, 200, 4, 'B', 1, kSym, P998));
    feed(sim, m);
    sim.finish();
    const SimResult& r = sim.result();
    // Only the buy at 10.02 is post-only-rejected; the post-only sell at 10.02
    // does not cross the 9.99 bid and rests.
    EXPECT_EQ(r.stats.rejected[static_cast<std::size_t>(Reject::post_only_would_cross)], 1u);
    EXPECT_EQ(r.stats.rejected[static_cast<std::size_t>(Reject::self_cross)], 1u);
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].report.qty, 100u);
    EXPECT_EQ(sim.exchange_open(1), 0u);  // cancelled at the end of the run
}

TEST(SimFills, NoFillsWhileTheStockIsHalted) {
    RecordingSchedule strat({place_at(100, Side::buy, P1000, 100)});
    Simulator sim(config(), strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P999));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1002));
    m.push_back(add(kLoc, 150, 3, 'B', 100, kSym, P1000));  // behind us
    m.push_back(trading_action(kLoc, 200, kSym, 'H'));
    m.push_back(executed(kLoc, 300, 3, 40));
    m.push_back(trading_action(kLoc, 400, kSym, 'T'));
    m.push_back(executed(kLoc, 500, 3, 60));
    feed(sim, m);
    sim.finish();
    ASSERT_EQ(sim.result().fills.size(), 1u);
    EXPECT_EQ(sim.result().fills[0].report.qty, 60u);
}

TEST(SimFills, RealOrderArrivingAtACrossingPriceFillsARestingShadow) {
    RecordingSchedule strat({place_at(100, Side::buy, P1001, 100)});  // inside the spread, nothing ahead
    Simulator sim(config(), strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P1000));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1002));
    m.push_back(add(kLoc, 200, 3, 'S', 60, kSym, P1001));   // would have traded with us
    m.push_back(add(kLoc, 300, 4, 'S', 100, kSym, P1000));  // also crosses us: the other 40
    feed(sim, m);
    sim.finish();
    const SimResult& r = sim.result();
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.fills[0].report.qty, 60u);
    EXPECT_EQ(r.fills[1].report.qty, 40u);
    EXPECT_EQ(r.fills[1].report.price, static_cast<Price>(P1001));  // our price: we were resting
    EXPECT_EQ(r.fills[0].trigger, 'A');
    EXPECT_EQ(r.fills[0].report.liquidity, Liquidity::maker);
}

TEST(SimNaive, FillsCompletelyTheFirstTimeTheMarketTradesAtItsPrice) {
    RecordingSchedule strat({place_at(100, Side::buy, P1000, 100), place_at(100, Side::sell, P1002, 100)});
    Simulator sim(config(0, 0, FillModel::naive_touch), strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 200, kSym, P1000));
    m.push_back(add(kLoc, 20, 2, 'B', 300, kSym, P1000));
    m.push_back(add(kLoc, 25, 3, 'S', 500, kSym, P1002));
    m.push_back(executed(kLoc, 200, 1, 10));  // 10 shares trade at 10.00, 500 ahead of us
    m.push_back(trade(kLoc, 300, 'S', 5, kSym, P1002));  // a hidden-order print at 10.02
    feed(sim, m);
    sim.finish();
    const SimResult& r = sim.result();
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.fills[0].report.qty, 100u);
    EXPECT_EQ(r.fills[0].report.exchange_ts, 200u);
    EXPECT_EQ(r.fills[1].report.qty, 100u);
    EXPECT_EQ(r.fills[1].trigger, 'P');
}

TEST(SimNaive, QueueAwareGivesNothingForTheSameStream) {
    RecordingSchedule strat({place_at(100, Side::buy, P1000, 100), place_at(100, Side::sell, P1002, 100)});
    Simulator sim(config(0, 0, FillModel::queue_aware), strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 200, kSym, P1000));
    m.push_back(add(kLoc, 20, 2, 'B', 300, kSym, P1000));
    m.push_back(add(kLoc, 25, 3, 'S', 500, kSym, P1002));
    m.push_back(executed(kLoc, 200, 1, 10));
    m.push_back(trade(kLoc, 300, 'S', 5, kSym, P1002));
    feed(sim, m);
    sim.finish();
    EXPECT_TRUE(sim.result().fills.empty());
}

// ---- Accounting and markouts ----------------------------------------------------

TEST(SimAccounting, MarkoutsAndMarkToMarketToTheMicroDollar) {
    const std::uint64_t s = 1'000'000'000;  // 1 second
    RecordingSchedule strat({place_at(1 * s, Side::buy, P1001, 100)});
    SimConfig cfg = config();
    cfg.end_ns = 30 * s;
    Simulator sim(cfg, strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P1000));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1002));                 // mid 10.01
    m.push_back(add(kLoc, 2 * s, 3, 'S', 100, kSym, P1001));              // fills us 100 @ 10.01
    m.push_back(executed(kLoc, 2 * s + 50'000'000, 3, 100));             // ask back to 10.02: mid 10.01
    m.push_back(add(kLoc, 2 * s + 500'000'000, 4, 'B', 100, kSym, P1001)); // mid 10.015
    m.push_back(del(kLoc, 7 * s, 4));
    m.push_back(del(kLoc, 7 * s, 1));
    m.push_back(add(kLoc, 7 * s, 5, 'B', 100, kSym, P998));              // mid 10.00
    m.push_back(add(kLoc, 25 * s, 6, 'S', 100, kSym, px(10.05)));
    m.push_back(add(kLoc, 40 * s, 7, 'S', 100, kSym, px(10.06)));
    feed(sim, m);
    sim.finish();
    const SimResult& r = sim.result();
    ASSERT_EQ(r.fills.size(), 1u);
    const FillRecord& f = r.fills[0];
    EXPECT_EQ(f.mid2_before, 2LL * px(10.01));
    ASSERT_TRUE(f.markout_ok[0] && f.markout_ok[1] && f.markout_ok[2]);
    EXPECT_DOUBLE_EQ(f.markout(0), 0.0);      // 100 ms: 10.01
    EXPECT_DOUBLE_EQ(f.markout(1), 0.005);    // 1 s: 10.015
    EXPECT_NEAR(f.markout(2), -0.01, 1e-12);  // 10 s: 10.00
    EXPECT_NEAR(f.markout_vs_price(2), -0.01, 1e-12);
    // Cash: -100 x $10.01 + $0.20 rebate; inventory 100 marked at $10.00.
    EXPECT_EQ(r.final_cash, -1'001'000'000 + 200'000);
    EXPECT_EQ(r.final_mid2, 2LL * px(10.00));
    EXPECT_EQ(r.final_mtm, -800'000);  // -$0.80
    EXPECT_EQ(r.end_ts, 30 * s);
}

TEST(SimAccounting, MarkoutPastTheLastMessageIsUnavailable) {
    RecordingSchedule strat({place_at(100, Side::buy, P1001, 100)});
    Simulator sim(config(), strat);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P1000));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1002));
    m.push_back(add(kLoc, 200, 3, 'S', 100, kSym, P1001));
    m.push_back(add(kLoc, 200'000'000, 4, 'B', 1, kSym, P998));  // 0.2 s: the 100 ms markout is measurable
    feed(sim, m);
    sim.finish();
    const SimResult& r = sim.result();
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_TRUE(r.fills[0].markout_ok[0]);
    EXPECT_FALSE(r.fills[0].markout_ok[1]);
    EXPECT_FALSE(r.fills[0].markout_ok[2]);
    EXPECT_EQ(r.stats.markouts_unavailable[1], 1u);
    EXPECT_EQ(r.stats.markouts_unavailable[2], 1u);
}

TEST(SimAccounting, ActionsAfterTheEndAreRejectedAndOrdersAreCancelledAtTheEnd) {
    LambdaStrategy s;
    ShadowId id = 0;
    s.start = [&](Context& c) { id = c.place(Side::buy, P1000, 100); };
    s.end = [&](Context& c) { EXPECT_EQ(c.place(Side::buy, P1000, 100), 0u); };
    SimConfig cfg = config();
    cfg.start_ns = 100;
    cfg.end_ns = 1000;
    Simulator sim(cfg, s);
    std::vector<Bytes> m = prologue();
    m.push_back(add(kLoc, 10, 1, 'B', 100, kSym, P999));
    m.push_back(add(kLoc, 20, 2, 'S', 100, kSym, P1002));
    m.push_back(add(kLoc, 2000, 3, 'B', 100, kSym, P1000));
    m.push_back(executed(kLoc, 3000, 3, 100));  // would have filled us, but the run ended at 1000
    feed(sim, m);
    sim.finish();
    EXPECT_TRUE(sim.result().fills.empty());
    EXPECT_EQ(sim.exchange_status(id), OrderStatus::cancelled);
    EXPECT_EQ(sim.result().stats.rejected[static_cast<std::size_t>(Reject::after_end)], 1u);
}
