// A small version of the differential fuzz test (Rule C), so every ctest run
// and every CI build compares the two engines on fresh random streams. The
// full-size run is tools/book_fuzz.

#include <gtest/gtest.h>

#include "tickforge/book/differential.hpp"

using namespace tickforge::book;

TEST(DifferentialFuzz, ShortStreamsAgreeAfterEveryEvent) {
    fuzz::Counters c;
    const auto cfg = fuzz::short_stream_config();
    for (std::uint64_t i = 0; i < 2000; ++i) {
        const auto m = fuzz::run_stream(1000 + i, cfg, c);
        ASSERT_FALSE(m.has_value()) << "seed " << m->seed << " event " << m->event << ": " << m->detail;
    }
    EXPECT_EQ(c.events, 2000u * cfg.events);
    // The stream really exercises matching and every kind of call.
    EXPECT_GT(c.fills, 1000u);
    for (std::size_t op = 0; op < fuzz::kOps; ++op) EXPECT_GT(c.by_op[op][0], 0u) << fuzz::to_string(static_cast<fuzz::Op>(op));
    EXPECT_GT(c.by_op[static_cast<int>(fuzz::Op::execute)][static_cast<int>(Status::too_many_shares)], 0u);
    EXPECT_GT(c.by_op[static_cast<int>(fuzz::Op::remove)][static_cast<int>(Status::unknown_order)], 0u);
    EXPECT_GT(c.by_op[static_cast<int>(fuzz::Op::add)][static_cast<int>(Status::duplicate_order)], 0u);
    EXPECT_GT(c.by_op[static_cast<int>(fuzz::Op::limit)][static_cast<int>(Status::bad_price)], 0u);
}

TEST(DifferentialFuzz, LongStreamWithDeepBookAgrees) {
    fuzz::Counters c;
    const auto m = fuzz::run_stream(77, fuzz::long_stream_config(20000), c);
    ASSERT_FALSE(m.has_value()) << "seed " << m->seed << " event " << m->event << ": " << m->detail;
    EXPECT_GT(c.max_orders, 300u);
    EXPECT_GT(c.max_levels, 100u);
}

// The harness must notice a broken engine; otherwise "no mismatches" proves
// nothing. This engine wraps the FastBook but rests every order with one
// share too few.
namespace {
class OffByOneBook : public FastBook {
public:
    Status add(OrderId id, Side s, Price p, Qty q) { return FastBook::add(id, s, p, q > 1 ? q - 1 : q); }
};
}  // namespace

TEST(DifferentialFuzz, DetectsADeliberatelyBrokenEngine) {
    fuzz::Counters c;
    bool caught = false;
    for (std::uint64_t i = 0; i < 50 && !caught; ++i)
        caught = fuzz::run_stream<OffByOneBook, ReferenceBook>(i, fuzz::short_stream_config(), c).has_value();
    EXPECT_TRUE(caught);
}
