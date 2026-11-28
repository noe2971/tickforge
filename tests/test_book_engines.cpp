// Behaviour tests run against BOTH engines (typed tests), so the fast engine
// and the reference engine are held to the same hand-written expectations,
// not only to each other.

#include <gtest/gtest.h>

#include <vector>

#include "tickforge/book/fast_book.hpp"
#include "tickforge/book/reference_book.hpp"

using namespace tickforge::book;

namespace {

template <typename Book>
class BookTest : public ::testing::Test {
protected:
    Book book;
    std::vector<Fill> fills;
    auto sink() {
        return [this](const Fill& f) { fills.push_back(f); };
    }
    std::vector<LevelView> levels(Side s) {
        std::vector<LevelView> out;
        book.for_each_level(s, [&](const LevelView& l) { out.push_back(l); });
        return out;
    }
    // Order ids at one side, best level first, queue order.
    std::vector<OrderId> queue(Side s) {
        std::vector<OrderId> out;
        book.for_each_order(s, [&](Price, OrderId id, Qty) { out.push_back(id); });
        return out;
    }
};

using Engines = ::testing::Types<FastBook, ReferenceBook>;
TYPED_TEST_SUITE(BookTest, Engines);

constexpr Side B = Side::buy;
constexpr Side S = Side::sell;

}  // namespace

// ---- Replay mode -----------------------------------------------------------

TYPED_TEST(BookTest, EmptyBookHasNoBestPrices) {
    EXPECT_FALSE(this->book.best(B).has_value());
    EXPECT_FALSE(this->book.best(S).has_value());
    EXPECT_EQ(this->book.order_count(), 0u);
    EXPECT_FALSE(this->book.find(1).has_value());
}

TYPED_TEST(BookTest, AddBuildsLevelsSortedBestFirst) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, B, 100, 10), Status::ok);
    ASSERT_EQ(b.add(2, B, 102, 20), Status::ok);
    ASSERT_EQ(b.add(3, B, 101, 30), Status::ok);
    ASSERT_EQ(b.add(4, B, 102, 5), Status::ok);
    ASSERT_EQ(b.add(5, S, 105, 7), Status::ok);
    ASSERT_EQ(b.add(6, S, 103, 8), Status::ok);
    ASSERT_EQ(b.add(7, S, 104, 9), Status::ok);

    const std::vector<LevelView> bids{{102, 25, 2}, {101, 30, 1}, {100, 10, 1}};
    const std::vector<LevelView> asks{{103, 8, 1}, {104, 9, 1}, {105, 7, 1}};
    EXPECT_EQ(this->levels(B), bids);
    EXPECT_EQ(this->levels(S), asks);
    EXPECT_EQ(b.best(B), (LevelView{102, 25, 2}));
    EXPECT_EQ(b.best(S), (LevelView{103, 8, 1}));
    EXPECT_EQ(b.order_count(), 7u);
    EXPECT_EQ(b.level_count(B), 3u);
    EXPECT_EQ(b.level_count(S), 3u);
    // Within a level: time priority (2 arrived before 4).
    EXPECT_EQ(this->queue(B), (std::vector<OrderId>{2, 4, 3, 1}));
}

TYPED_TEST(BookTest, DepthReturnsAtMostNLevelsBestFirst) {
    auto& b = this->book;
    for (int i = 0; i < 5; ++i) ASSERT_EQ(b.add(10 + i, S, 200 + i, 1 + i), Status::ok);
    LevelView out[3];
    ASSERT_EQ(b.depth(S, out, 3), 3u);
    EXPECT_EQ(out[0], (LevelView{200, 1, 1}));
    EXPECT_EQ(out[2], (LevelView{202, 3, 1}));
    LevelView many[10];
    EXPECT_EQ(b.depth(S, many, 10), 5u);
    EXPECT_EQ(b.depth(B, many, 10), 0u);
}

TYPED_TEST(BookTest, AddRejectsBadInputAndLeavesBookUnchanged) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, B, 100, 10), Status::ok);
    EXPECT_EQ(b.add(1, S, 101, 10), Status::duplicate_order);
    EXPECT_EQ(b.add(2, B, 100, 0), Status::zero_quantity);
    EXPECT_EQ(b.add(3, B, 0, 10), Status::bad_price);
    EXPECT_EQ(b.add(4, B, -5, 10), Status::bad_price);
    // Duplicate is checked first, then shares, then price.
    EXPECT_EQ(b.add(1, B, 0, 0), Status::duplicate_order);
    EXPECT_EQ(b.add(5, B, 0, 0), Status::zero_quantity);
    EXPECT_EQ(b.order_count(), 1u);
    EXPECT_EQ(b.best(B), (LevelView{100, 10, 1}));
    EXPECT_FALSE(b.best(S).has_value());
}

TYPED_TEST(BookTest, ExecutePartialKeepsPriorityFullRemoves) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, S, 50, 100), Status::ok);
    ASSERT_EQ(b.add(2, S, 50, 40), Status::ok);

    EXPECT_EQ(b.execute(1, 30), (ExecResult{Status::ok, S, 50, 70}));
    EXPECT_EQ(b.best(S), (LevelView{50, 110, 2}));
    EXPECT_EQ(this->queue(S), (std::vector<OrderId>{1, 2}));  // still first
    EXPECT_TRUE(b.find(1)->at_front);

    EXPECT_EQ(b.execute(1, 70), (ExecResult{Status::ok, S, 50, 0}));
    EXPECT_FALSE(b.find(1).has_value());
    EXPECT_EQ(b.best(S), (LevelView{50, 40, 1}));
    EXPECT_TRUE(b.find(2)->at_front);

    EXPECT_EQ(b.execute(2, 40).remaining, 0u);
    EXPECT_FALSE(b.best(S).has_value());
    EXPECT_EQ(b.order_count(), 0u);
}

TYPED_TEST(BookTest, ExecuteErrorsReportOrderAndChangeNothing) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, B, 99, 10), Status::ok);
    EXPECT_EQ(b.execute(2, 5), (ExecResult{Status::unknown_order, B, 0, 0}));
    EXPECT_EQ(b.execute(1, 11), (ExecResult{Status::too_many_shares, B, 99, 10}));
    EXPECT_EQ(b.execute(1, 0), (ExecResult{Status::zero_quantity, B, 99, 10}));
    EXPECT_EQ(b.find(1), (OrderView{1, B, 99, 10, true}));
}

TYPED_TEST(BookTest, PartialCancelKeepsTimePriority) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, B, 100, 50), Status::ok);
    ASSERT_EQ(b.add(2, B, 100, 50), Status::ok);
    EXPECT_EQ(b.cancel(1, 20), Status::ok);
    EXPECT_EQ(b.find(1), (OrderView{1, B, 100, 30, true}));
    EXPECT_EQ(this->queue(B), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(b.cancel(1, 31), Status::too_many_shares);
    EXPECT_EQ(b.cancel(1, 30), Status::ok);  // all remaining shares: order gone
    EXPECT_FALSE(b.find(1).has_value());
    EXPECT_EQ(b.cancel(1, 1), Status::unknown_order);
    EXPECT_EQ(b.best(B), (LevelView{100, 50, 1}));
}

TYPED_TEST(BookTest, RemoveFromMiddleOfQueueAndEmptyLevels) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, S, 10, 1), Status::ok);
    ASSERT_EQ(b.add(2, S, 10, 2), Status::ok);
    ASSERT_EQ(b.add(3, S, 10, 3), Status::ok);
    ASSERT_EQ(b.add(4, S, 11, 4), Status::ok);
    ASSERT_EQ(b.add(5, S, 12, 5), Status::ok);
    EXPECT_EQ(b.remove(2), Status::ok);  // middle of the queue
    EXPECT_EQ(this->queue(S), (std::vector<OrderId>{1, 3, 4, 5}));
    EXPECT_EQ(b.remove(4), Status::ok);  // a whole non-best level disappears
    EXPECT_EQ(this->levels(S), (std::vector<LevelView>{{10, 4, 2}, {12, 5, 1}}));
    EXPECT_EQ(b.remove(1), Status::ok);
    EXPECT_EQ(b.remove(3), Status::ok);  // best level disappears
    EXPECT_EQ(b.best(S), (LevelView{12, 5, 1}));
    EXPECT_EQ(b.remove(3), Status::unknown_order);
}

TYPED_TEST(BookTest, ReplaceLosesTimePriorityAndKeepsSide) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, B, 100, 10), Status::ok);
    ASSERT_EQ(b.add(2, B, 100, 10), Status::ok);
    // Same price, new size: order 1 becomes 3 and goes behind order 2.
    EXPECT_EQ(b.replace(1, 3, 100, 15), Status::ok);
    EXPECT_FALSE(b.find(1).has_value());
    EXPECT_EQ(b.find(3), (OrderView{3, B, 100, 15, false}));
    EXPECT_EQ(this->queue(B), (std::vector<OrderId>{2, 3}));
    // New price: moves to its own level.
    EXPECT_EQ(b.replace(2, 4, 101, 5), Status::ok);
    EXPECT_EQ(this->levels(B), (std::vector<LevelView>{{101, 5, 1}, {100, 15, 1}}));
}

TYPED_TEST(BookTest, ReplaceErrorsInOrderAndChangeNothing) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, S, 100, 10), Status::ok);
    ASSERT_EQ(b.add(2, S, 101, 10), Status::ok);
    EXPECT_EQ(b.replace(9, 3, 100, 1), Status::unknown_order);
    EXPECT_EQ(b.replace(1, 2, 100, 1), Status::duplicate_order);
    EXPECT_EQ(b.replace(1, 1, 100, 1), Status::duplicate_order);  // new id must differ
    EXPECT_EQ(b.replace(1, 3, 100, 0), Status::zero_quantity);
    EXPECT_EQ(b.replace(1, 3, 0, 1), Status::bad_price);
    EXPECT_EQ(b.order_count(), 2u);
    EXPECT_EQ(b.find(1), (OrderView{1, S, 100, 10, true}));
}

TYPED_TEST(BookTest, ReplayAddsNeverMatchEvenIfCrossing) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, S, 100, 10), Status::ok);
    ASSERT_EQ(b.add(2, B, 105, 10), Status::ok);  // crosses: replay mode trusts the feed
    EXPECT_EQ(b.best(B)->price, 105);
    EXPECT_EQ(b.best(S)->price, 100);
    EXPECT_EQ(b.order_count(), 2u);
}

// ---- Matching mode -----------------------------------------------------------

TYPED_TEST(BookTest, LimitOrderThatDoesNotCrossRests) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, S, 101, 10), Status::ok);
    const auto r = b.submit_limit(2, B, 100, 5, this->sink());
    EXPECT_EQ(r, (SubmitResult{Status::ok, 0, 5}));
    EXPECT_TRUE(this->fills.empty());
    EXPECT_EQ(b.best(B), (LevelView{100, 5, 1}));
}

TYPED_TEST(BookTest, PriceThenTimePriorityAcrossLevels) {
    auto& b = this->book;
    // Asks: 101 x [10 (id1), 20 (id2)], 102 x [30 (id3)], 103 x [40 (id4)]
    ASSERT_EQ(b.add(1, S, 101, 10), Status::ok);
    ASSERT_EQ(b.add(3, S, 102, 30), Status::ok);
    ASSERT_EQ(b.add(2, S, 101, 20), Status::ok);
    ASSERT_EQ(b.add(4, S, 103, 40), Status::ok);

    // Buy 45 up to 102: takes all of 101 (oldest first), then 15 of 102.
    const auto r = b.submit_limit(9, B, 102, 45, this->sink());
    EXPECT_EQ(r, (SubmitResult{Status::ok, 45, 0}));
    const std::vector<Fill> want{{1, 9, 101, 10}, {2, 9, 101, 20}, {3, 9, 102, 15}};
    EXPECT_EQ(this->fills, want);
    EXPECT_EQ(this->levels(S), (std::vector<LevelView>{{102, 15, 1}, {103, 40, 1}}));
    EXPECT_FALSE(b.find(9).has_value());  // fully filled: does not rest
}

TYPED_TEST(BookTest, LimitStopsAtItsPriceAndRestsRemainder) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, B, 100, 10), Status::ok);
    ASSERT_EQ(b.add(2, B, 99, 10), Status::ok);
    const auto r = b.submit_limit(3, S, 100, 25, this->sink());
    EXPECT_EQ(r, (SubmitResult{Status::ok, 10, 15}));
    EXPECT_EQ(this->fills, (std::vector<Fill>{{1, 3, 100, 10}}));
    // Remainder rests as the new best ask at 100; bid 99 untouched.
    EXPECT_EQ(b.best(S), (LevelView{100, 15, 1}));
    EXPECT_EQ(b.best(B), (LevelView{99, 10, 1}));
    EXPECT_EQ(b.find(3), (OrderView{3, S, 100, 15, true}));
}

TYPED_TEST(BookTest, FillsAreAtTheRestingPrice) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, S, 100, 10), Status::ok);
    b.submit_limit(2, B, 150, 4, this->sink());  // willing to pay 150, trades at 100
    EXPECT_EQ(this->fills, (std::vector<Fill>{{1, 2, 100, 4}}));
    EXPECT_EQ(b.find(1), (OrderView{1, S, 100, 6, true}));
}

TYPED_TEST(BookTest, PartialFillOfMakerKeepsItAtFront) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, B, 100, 50), Status::ok);
    ASSERT_EQ(b.add(2, B, 100, 50), Status::ok);
    b.submit_limit(3, S, 100, 20, this->sink());
    EXPECT_EQ(this->fills, (std::vector<Fill>{{1, 3, 100, 20}}));
    EXPECT_EQ(this->queue(B), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(b.best(B), (LevelView{100, 80, 2}));
}

TYPED_TEST(BookTest, MarketOrderSweepsAndNeverRests) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, S, 101, 10), Status::ok);
    ASSERT_EQ(b.add(2, S, 105, 10), Status::ok);
    const auto r = b.submit_market(3, B, 25, this->sink());
    EXPECT_EQ(r, (SubmitResult{Status::ok, 20, 0}));
    EXPECT_EQ(this->fills, (std::vector<Fill>{{1, 3, 101, 10}, {2, 3, 105, 10}}));
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_FALSE(b.best(B).has_value());  // the unfilled 5 were not rested
}

TYPED_TEST(BookTest, MarketOrderOnEmptySideFillsNothing) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, B, 100, 10), Status::ok);
    EXPECT_EQ(b.submit_market(2, B, 10, this->sink()), (SubmitResult{Status::ok, 0, 0}));
    EXPECT_TRUE(this->fills.empty());
    EXPECT_EQ(b.order_count(), 1u);
}

TYPED_TEST(BookTest, SubmitRejectsBadInputBeforeMatching) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, S, 100, 10), Status::ok);
    EXPECT_EQ(b.submit_limit(1, B, 100, 5, this->sink()).status, Status::duplicate_order);
    EXPECT_EQ(b.submit_limit(2, B, 100, 0, this->sink()).status, Status::zero_quantity);
    EXPECT_EQ(b.submit_limit(2, B, 0, 5, this->sink()).status, Status::bad_price);
    EXPECT_EQ(b.submit_market(1, B, 5, this->sink()).status, Status::duplicate_order);
    EXPECT_EQ(b.submit_market(2, B, 0, this->sink()).status, Status::zero_quantity);
    EXPECT_TRUE(this->fills.empty());
    EXPECT_EQ(b.find(1), (OrderView{1, S, 100, 10, true}));
}

TYPED_TEST(BookTest, CancelInMatchingModeAndReuseOfIdAfterDeath) {
    auto& b = this->book;
    ASSERT_EQ(b.submit_limit(1, B, 100, 10, this->sink()).rested, 10u);
    EXPECT_EQ(b.remove(1), Status::ok);
    EXPECT_FALSE(b.best(B).has_value());
    // Once an id is no longer live it may be used again.
    EXPECT_EQ(b.submit_limit(1, S, 100, 10, this->sink()).rested, 10u);
    EXPECT_EQ(b.find(1), (OrderView{1, S, 100, 10, true}));
}

TYPED_TEST(BookTest, ManyOrdersAtManyLevelsStayConsistent) {
    auto& b = this->book;
    // Interleave prices so levels are inserted in the middle of the array.
    OrderId id = 1;
    for (int round = 0; round < 3; ++round)
        for (Price p : {500, 300, 700, 100, 900, 400, 600, 200, 800})
            ASSERT_EQ(b.add(id++, B, p, static_cast<Qty>(p)), Status::ok);
    auto lv = this->levels(B);
    ASSERT_EQ(lv.size(), 9u);
    for (std::size_t i = 0; i < lv.size(); ++i) {
        EXPECT_EQ(lv[i].price, static_cast<Price>(900 - 100 * i));
        EXPECT_EQ(lv[i].orders, 3u);
        EXPECT_EQ(lv[i].shares, static_cast<std::uint64_t>(3 * lv[i].price));
    }
    // A big sell sweeps everything down to 400 inclusive.
    const auto r = b.submit_limit(id, S, 400, 1'000'000, this->sink());
    EXPECT_EQ(r.filled, 3u * (900 + 800 + 700 + 600 + 500 + 400));
    EXPECT_EQ(b.best(B)->price, 300);
    EXPECT_EQ(b.best(S), (LevelView{400, 1'000'000 - r.filled, 1}));
}

// FastBook-only helper used by the queue-jump analysis and the simulator.
TEST(FastBookLevelWalk, ForEachOrderAtVisitsOneLevelInQueueOrder) {
    FastBook b;
    ASSERT_EQ(b.add(1, Side::buy, 1000, 10), Status::ok);
    ASSERT_EQ(b.add(2, Side::buy, 1001, 20), Status::ok);
    ASSERT_EQ(b.add(3, Side::buy, 1000, 30), Status::ok);
    ASSERT_EQ(b.add(4, Side::sell, 1000, 40), Status::ok);  // same price, other side
    ASSERT_EQ(b.add(5, Side::buy, 1000, 50), Status::ok);
    ASSERT_EQ(b.cancel(3, 5), Status::ok);                   // keeps its place
    std::vector<std::pair<OrderId, Qty>> seen;
    b.for_each_order_at(Side::buy, 1000, [&](OrderId id, Qty q) { seen.emplace_back(id, q); });
    const std::vector<std::pair<OrderId, Qty>> want{{1, 10}, {3, 25}, {5, 50}};
    EXPECT_EQ(seen, want);
    seen.clear();
    b.for_each_order_at(Side::sell, 1000, [&](OrderId id, Qty q) { seen.emplace_back(id, q); });
    EXPECT_EQ(seen, (std::vector<std::pair<OrderId, Qty>>{{4, 40}}));
    seen.clear();
    b.for_each_order_at(Side::buy, 999, [&](OrderId id, Qty q) { seen.emplace_back(id, q); });  // no such level
    b.for_each_order_at(Side::sell, 2000, [&](OrderId id, Qty q) { seen.emplace_back(id, q); });
    EXPECT_TRUE(seen.empty());
}
