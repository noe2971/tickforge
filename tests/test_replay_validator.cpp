// ReplayValidator and ItchReplay on small hand-built days, where the right
// answer for every check is known in advance.

#include <gtest/gtest.h>

#include <sstream>

#include "itch_book_builders.hpp"
#include "tickforge/book/itch_replay.hpp"
#include "tickforge/book/reference_book.hpp"
#include "tickforge/book/replay_validator.hpp"

using namespace tickforge;
using namespace tickforge::book;
using namespace tickforge::test::itchmsg;
using tickforge::test::Bytes;

namespace {

constexpr std::uint64_t kOpen = 34'200'000'000'000;   // 09:30:00
constexpr std::uint64_t kClose = 57'600'000'000'000;  // 16:00:00

// A clean little day: every message consistent, two orders left at the end.
std::vector<Bytes> clean_day() {
    return {
        system_event(1000, 'O'),
        directory(7, 2000, "ABC"),
        trading_action(7, 2500, "ABC", 'T'),
        add(7, 3000, 1, 'B', 100, "ABC", 100000),       // bid 10.00 x100
        add_mpid(7, 3100, 2, 'B', 50, "ABC", 99000),    // bid  9.90 x50
        add(7, 3200, 3, 'S', 200, "ABC", 101000),       // ask 10.10 x200
        system_event(kOpen, 'Q'),
        executed(7, kOpen + 10, 1, 30),                 // bid 10.00 now x70 (at best: ok)
        cancel(7, kOpen + 20, 3, 50),                   // ask now x150
        replace(7, kOpen + 30, 2, 4, 60, 99500),        // bid 9.90 x50 -> bid 9.95 x60 (ref 4)
        trade(7, kOpen + 35, 'B', 300, "ABC", 100500),  // hidden order trade: no book change
        executed_price(7, kOpen + 40, 1, 70, 'Y', 100000),  // C at the same price: bid 10.00 gone
        executed(7, kOpen + 50, 4, 10),                 // best bid is now 9.95 (ref 4): at best
        del(7, kOpen + 60, 3),                          // ask gone
        add(7, kOpen + 70, 5, 'S', 40, "ABC", 100200),  // ask 10.02 x40
        system_event(kClose, 'M'),
        cross(7, kClose + 1, 1000, "ABC", 100100, 'C'),
        system_event(kClose + 2, 'C'),
    };
}

}  // namespace

TEST(ReplayValidator, CleanDayHasNoExceptionsAndReconciles) {
    ReplayValidator v;
    feed(v, clean_day());
    v.finish();
    EXPECT_EQ(v.issue_total(), 0u);
    const SymbolStats* s = v.symbol(7);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->added_shares, 100u + 50 + 200 + 60 + 40);
    EXPECT_EQ(s->executed_shares, 30u + 70 + 10);
    EXPECT_EQ(s->cancelled_shares, 50u);
    EXPECT_EQ(s->deleted_shares, 150u);
    EXPECT_EQ(s->replaced_shares, 50u);
    EXPECT_EQ(s->eod_book_orders, 2u);  // ref 4 (50 left) and ref 5 (40)
    EXPECT_EQ(s->eod_book_shares, 90u);
    EXPECT_EQ(s->eod_ledger_shares, 90u);
    EXPECT_EQ(s->peak_orders, 3u);
    EXPECT_EQ(s->min_price, 99000);
    EXPECT_EQ(s->max_price, 101000);
    std::ostringstream md, csv;
    v.write_markdown(md, "clean", {"ABC"});
    v.write_peaks_csv(csv);
    EXPECT_NE(md.str().find("| Stocks whose shares reconcile exactly | 1 |"), std::string::npos) << md.str();
    EXPECT_NE(csv.str().find("7,ABC,3,2,1,"), std::string::npos) << csv.str();
}

TEST(ReplayValidator, EveryKindOfBadMessageIsCountedNotDropped) {
    ReplayValidator v;
    feed(v, {
                system_event(1000, 'O'),
                directory(7, 2000, "ABC"),
                directory(8, 2001, "XYZ"),
                add(7, 3000, 1, 'B', 100, "ABC", 100000),
                add(7, 3100, 2, 'B', 100, "ABC", 99000),
                add(8, 3200, 9, 'S', 10, "XYZ", 50000),
                executed(7, 4000, 77, 10),           // unknown order
                executed(7, 4100, 1, 101),           // too many shares
                executed(7, 4200, 2, 10),            // not at best (best bid is 10.00)
                cancel(7, 4300, 77, 1),              // unknown
                cancel(7, 4400, 1, 1000),            // too many
                del(7, 4500, 77),                    // unknown
                replace(7, 4600, 77, 78, 1, 1),      // unknown original
                replace(7, 4700, 1, 2, 10, 100000),  // new ref already live
                add(7, 4800, 1, 'S', 5, "ABC", 1),   // duplicate reference
                add(7, 4900, 3, 'B', 0, "ABC", 1),   // zero shares
                executed(7, 5000, 9, 1),             // ref 9 lives in XYZ, not ABC
                del(7, 4000, 2),                     // timestamp goes backwards (valid delete)
            });
    v.finish();
    EXPECT_EQ(v.issue(Issue::exec_unknown_order).count, 1u);
    EXPECT_EQ(v.issue(Issue::exec_too_many_shares).count, 1u);
    EXPECT_EQ(v.issue(Issue::exec_not_at_best).count, 1u);
    EXPECT_EQ(v.issue(Issue::cancel_unknown_order).count, 1u);
    EXPECT_EQ(v.issue(Issue::cancel_too_many_shares).count, 1u);
    EXPECT_EQ(v.issue(Issue::delete_unknown_order).count, 1u);
    EXPECT_EQ(v.issue(Issue::replace_unknown_order).count, 1u);
    EXPECT_EQ(v.issue(Issue::replace_duplicate_new).count, 1u);
    EXPECT_EQ(v.issue(Issue::add_duplicate_order).count, 1u);
    EXPECT_EQ(v.issue(Issue::bad_values).count, 1u);
    EXPECT_EQ(v.issue(Issue::ref_in_other_stock).count, 1u);
    EXPECT_EQ(v.issue(Issue::timestamp_backwards).count, 1u);
    // Bad messages change nothing, so the shares still reconcile.
    EXPECT_EQ(v.issue(Issue::ledger_mismatch).count, 0u);
    EXPECT_EQ(v.issue(Issue::reconciliation_failure).count, 0u);
    EXPECT_EQ(v.symbol(7)->executed_shares, 10u);  // only the not-at-best one was applied
    ASSERT_EQ(v.issue(Issue::exec_not_at_best).examples.size(), 1u);
    EXPECT_EQ(v.issue(Issue::exec_not_at_best).examples[0].ref, 2u);
}

TEST(ReplayValidator, CrossedAndLockedBooksAreFoundOnlyInContinuousTrading) {
    ReplayValidator v;
    feed(v, {
                system_event(1000, 'O'),
                directory(7, 2000, "ABC"),
                trading_action(7, 2100, "ABC", 'T'),
                add(7, 3000, 1, 'S', 100, "ABC", 100000),
                add(7, 3100, 2, 'B', 100, "ABC", 100100),  // crossed before the open: not an exception
                del(7, 3200, 2),
                system_event(kOpen, 'Q'),
                add(7, kOpen + 10, 3, 'B', 100, "ABC", 100100),  // crossed in continuous trading
                del(7, kOpen + 10, 3),                            // ... resolved at the same nanosecond
                add(7, kOpen + 20, 4, 'B', 100, "ABC", 100000),  // locked
                add(7, kOpen + 21, 5, 'B', 100, "ABC", 99000),   // still locked (another message)
                del(7, kOpen + 1020, 4),                          // resolved 1000 ns later
                trading_action(7, kOpen + 2000, "ABC", 'H'),      // halted
                add(7, kOpen + 3000, 6, 'B', 100, "ABC", 100200),  // crossed while halted: outside
                del(7, kOpen + 4000, 6),
            });
    v.finish();
    EXPECT_EQ(v.issue(Issue::crossed_continuous).count, 1u);
    EXPECT_EQ(v.issue(Issue::locked_continuous).count, 1u);
    const CrossStats& cc = v.cross_stats(true, true);
    EXPECT_EQ(cc.episodes, 1u);
    EXPECT_EQ(cc.same_timestamp, 1u);
    EXPECT_EQ(cc.duration[0], 1u);
    const CrossStats& lc = v.cross_stats(false, true);
    EXPECT_EQ(lc.episodes, 1u);
    EXPECT_EQ(lc.messages, 2u);
    EXPECT_EQ(lc.duration[2], 1u);  // 1000 ns: in [1 us, 1 ms)
    EXPECT_EQ(v.cross_stats(true, false).episodes, 2u);  // pre-open and halted
}

TEST(ItchReplay, BothEnginesBuildTheSameBooksFromTheSameDay) {
    ItchReplay<FastBook> fast;
    ItchReplay<ReferenceBook> ref;
    feed(fast, clean_day());
    feed(ref, clean_day());
    EXPECT_EQ(fast.book_messages(), 10u);
    EXPECT_EQ(fast.results(), ref.results());
    EXPECT_EQ(book_digest(fast.books()), book_digest(ref.books()));
    const FastBook* b = fast.books().find(7);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->best(Side::buy), (LevelView{99500, 50, 1}));
    EXPECT_EQ(b->best(Side::sell), (LevelView{100200, 40, 1}));
}

TEST(ItchReplay, CountsResultsByKindAndStatus) {
    ItchReplay<FastBook> r;
    feed(r, {directory(7, 1, "ABC"), add(7, 2, 1, 'B', 10, "ABC", 100), add(7, 3, 1, 'B', 10, "ABC", 100),
             del(7, 4, 2), executed(7, 5, 1, 10)});
    const auto& res = r.results();
    EXPECT_EQ(res[static_cast<int>(OpKind::add)][static_cast<int>(Status::ok)], 1u);
    EXPECT_EQ(res[static_cast<int>(OpKind::add)][static_cast<int>(Status::duplicate_order)], 1u);
    EXPECT_EQ(res[static_cast<int>(OpKind::remove)][static_cast<int>(Status::unknown_order)], 1u);
    EXPECT_EQ(res[static_cast<int>(OpKind::execute)][static_cast<int>(Status::ok)], 1u);
    EXPECT_EQ(r.books().find(7)->order_count(), 0u);
}

TEST(ReplayValidator, PostHocBreakdownsDescribeQueuePositionAndPriceGaps) {
    ReplayValidator v;
    feed(v, {
                system_event(1000, 'O'),
                directory(7, 2000, "ABC"),
                add(7, 3000, 1, 'B', 100, "ABC", 100000),
                add(7, 3100, 2, 'B', 100, "ABC", 100000),
                replace(7, 3200, 2, 3, 50, 100000),               // same price, fewer shares
                executed(7, 3300, 3, 10),                         // not first in queue: position 2
                executed_price(7, 3400, 1, 10, 'Y', 100100),      // one cent away from the resting price
            });
    v.finish();
    EXPECT_EQ(v.issue_total(), 0u);
    std::ostringstream md;
    v.write_markdown(md, "posthoc", {"ABC"});
    const std::string s = md.str();
    EXPECT_NE(s.find("## 6. Post-hoc"), std::string::npos);
    EXPECT_NE(s.find("| Y | 0 | 0 | 0 | 1 | 0 | 0 |"), std::string::npos) << s;
    EXPECT_NE(s.find("| U, same price, fewer shares | 1 | 100.0000% |"), std::string::npos) << s;
    EXPECT_NE(s.find("position 2, 100 sh ahead; executed order: U same px fewer sh; front order: add"), std::string::npos) << s;
}
