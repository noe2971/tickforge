// QueueJumpAnalyzer on hand-built streams where the right classification of
// every queue jump is known in advance.

#include <gtest/gtest.h>

#include <sstream>

#include "itch_book_builders.hpp"
#include "tickforge/book/queue_jumps.hpp"

using namespace tickforge;
using namespace tickforge::book;
using namespace tickforge::test::itchmsg;

namespace {
constexpr std::uint64_t kOpen = 34'200'000'000'000;  // 09:30:00
}

TEST(QueueJumps, FrontExecutionIsNotAJump) {
    QueueJumpAnalyzer a;
    feed(a, {directory(7, 1, "ABC"), system_event(kOpen, 'Q'), add(7, kOpen + 1, 1, 'B', 100, "ABC", 100000),
             add(7, kOpen + 2, 2, 'B', 100, "ABC", 100000), executed(7, kOpen + 3, 1, 100),
             executed(7, kOpen + 4, 2, 50)});
    a.finish();
    EXPECT_EQ(a.executions(), 2u);
    EXPECT_EQ(a.front_executions(), 2u);
    EXPECT_TRUE(a.jumps().empty());
}

TEST(QueueJumps, SameTimestampReorderIsClassifiedAsAllExecuted) {
    // Queue 1, 2, 3; one sweep at t=10 is published as E 3, E 1, E 2.
    QueueJumpAnalyzer a;
    feed(a, {directory(7, 1, "ABC"), system_event(kOpen, 'Q'), add(7, kOpen + 1, 1, 'S', 100, "ABC", 100100),
             add(7, kOpen + 2, 2, 'S', 40, "ABC", 100100), add(7, kOpen + 3, 3, 'S', 200, "ABC", 100100),
             executed(7, kOpen + 10, 3, 200), executed(7, kOpen + 10, 1, 100), executed(7, kOpen + 10, 2, 40),
             add(7, kOpen + 11, 4, 'S', 10, "ABC", 100100)});
    a.finish();
    ASSERT_EQ(a.jumps().size(), 1u);  // E 3 (position 3); E 1 is then at the front; E 2 too
    const QueueJump& j = a.jumps()[0];
    EXPECT_EQ(j.ref, 3u);
    EXPECT_EQ(j.position, 3u);
    EXPECT_EQ(j.ahead_shares, 140u);
    EXPECT_EQ(j.cls, JumpClass::all_ahead_executed_same_ts);
    EXPECT_FALSE(j.in_cross);
    EXPECT_TRUE(j.market_hours);
}

TEST(QueueJumps, RemovalAtSameTimestampIsAllGone) {
    QueueJumpAnalyzer a;
    feed(a, {directory(7, 1, "ABC"), add(7, 5, 1, 'B', 100, "ABC", 100000), add(7, 6, 2, 'B', 100, "ABC", 100000),
             executed(7, 10, 2, 100), del(7, 10, 1)});
    a.finish();
    ASSERT_EQ(a.jumps().size(), 1u);
    EXPECT_EQ(a.jumps()[0].cls, JumpClass::all_ahead_gone_same_ts);
    EXPECT_FALSE(a.jumps()[0].market_hours);
}

TEST(QueueJumps, SkippedOrderThatStaysIsFollowedToItsNextEvent) {
    QueueJumpAnalyzer a;
    feed(a, {directory(7, 1, "ABC"), add_mpid(7, 5, 1, 'B', 40, "ABC", 100000), add(7, 6, 2, 'B', 100, "ABC", 100000),
             add(7, 7, 3, 'S', 100, "ABC", 100100), executed(7, 10, 2, 60),
             executed_price(7, 2'000, 1, 40, 'Y', 100100)});  // later: C one cent more aggressive
    a.finish();
    ASSERT_EQ(a.jumps().size(), 1u);
    const QueueJump& j = a.jumps()[0];
    EXPECT_EQ(j.cls, JumpClass::some_ahead_remain);
    ASSERT_EQ(j.ahead.size(), 1u);
    const SkippedOrder& s = j.ahead[0];
    EXPECT_EQ(s.id, 1u);
    EXPECT_FALSE(s.gone_same_ts);
    EXPECT_EQ(s.next, Fate::executed_c);
    EXPECT_EQ(s.next_dt_ns, 1'990u);
    EXPECT_EQ(s.next_c_rel, 1);  // a bid executed above its displayed price
    EXPECT_EQ(s.info.kind, 'F');
    EXPECT_EQ(s.info.added_shares, 40u);
    EXPECT_FALSE(j.fully_executed);
}

TEST(QueueJumps, CrossAtSameTimestampIsFlaggedAndEndOfFileIsAFate) {
    QueueJumpAnalyzer a;
    feed(a, {directory(7, 1, "ABC"), add(7, 5, 1, 'B', 100, "ABC", 100000), add(7, 6, 2, 'B', 100, "ABC", 100000),
             executed(7, 10, 2, 100), cross(7, 10, 100, "ABC", 100000, 'O')});
    a.finish();
    ASSERT_EQ(a.jumps().size(), 1u);
    EXPECT_TRUE(a.jumps()[0].in_cross);
    EXPECT_EQ(a.jumps()[0].ahead[0].next, Fate::end_of_day);
    std::ostringstream md;
    a.write_markdown(md, "test");
    EXPECT_NE(md.str().find("queue jumps) | 1 (100.00%)"), std::string::npos) << md.str();
}
