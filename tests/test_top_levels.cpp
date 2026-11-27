// top_levels_at: the book just before time T, never anything from T onwards.

#include <gtest/gtest.h>

#include "itch_book_builders.hpp"
#include "tickforge/book/top_levels.hpp"
#include "tickforge/itch/stream.hpp"

using namespace tickforge;
using namespace tickforge::book;
using namespace tickforge::test::itchmsg;
using tickforge::test::TempFile;

namespace {

std::vector<tickforge::test::Bytes> day() {
    return {
        system_event(10, 'O'),
        directory(3, 20, "OTHER"),
        directory(7, 30, "ABC"),
        add(7, 100, 1, 'B', 10, "ABC", 5000),
        add(3, 150, 50, 'B', 999, "OTHER", 5000),  // another stock: ignored
        add(7, 200, 2, 'B', 20, "ABC", 5100),
        add(7, 200, 3, 'S', 30, "ABC", 5300),
        add(7, 300, 4, 'S', 40, "ABC", 5200),
        executed(7, 400, 4, 15),
        add(7, 500, 5, 'B', 5, "ABC", 4900),
    };
}

}  // namespace

TEST(TopLevels, AppliesOnlyMessagesStrictlyBeforeT) {
    TempFile f("day.itch");
    write_file(f.path, day());

    const auto at200 = top_levels_at(f.path, "ABC", 200);
    EXPECT_EQ(at200.messages_applied, 1u);  // only the add at t=100; both adds at t=200 are excluded
    EXPECT_EQ(at200.last_applied_ns, 100u);
    EXPECT_EQ(at200.bids, (std::vector<LevelView>{{5000, 10, 1}}));
    EXPECT_TRUE(at200.asks.empty());

    const auto at201 = top_levels_at(f.path, "ABC", 201);
    EXPECT_EQ(at201.messages_applied, 3u);
    EXPECT_EQ(at201.bids, (std::vector<LevelView>{{5100, 20, 1}, {5000, 10, 1}}));
    EXPECT_EQ(at201.asks, (std::vector<LevelView>{{5300, 30, 1}}));
}

TEST(TopLevels, ManyTimesInOnePassComeBackInRequestedOrder) {
    TempFile f("day.itch");
    write_file(f.path, day());
    const auto r = top_levels_at(f.path, "ABC", {401, 0, 1'000'000});
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].before_ns, 401u);
    EXPECT_EQ(r[0].asks, (std::vector<LevelView>{{5200, 25, 1}, {5300, 30, 1}}));
    EXPECT_EQ(r[1].messages_applied, 0u);  // before anything
    EXPECT_TRUE(r[1].bids.empty());
    EXPECT_EQ(r[2].messages_applied, 6u);  // after the last message: whole day
    EXPECT_EQ(r[2].bids.size(), 3u);
}

TEST(TopLevels, LimitsLevelsAndBothEnginesAgree) {
    TempFile f("day.itch");
    write_file(f.path, day());
    const auto fast = top_levels_at(f.path, "ABC", 1'000'000, 2, Engine::fast);
    const auto ref = top_levels_at(f.path, "ABC", 1'000'000, 2, Engine::reference);
    EXPECT_EQ(fast.bids.size(), 2u);
    EXPECT_EQ(fast.bids.front().price, 5100);
    EXPECT_EQ(fast, ref);
}

TEST(TopLevels, UnknownSymbolThrows) {
    TempFile f("day.itch");
    write_file(f.path, day());
    EXPECT_THROW(top_levels_at(f.path, "NOPE", 100), std::runtime_error);
}

TEST(TopLevels, ParseTimeOfDay) {
    EXPECT_EQ(parse_time_of_day("09:30:00"), 34'200'000'000'000u);
    EXPECT_EQ(parse_time_of_day("09:30:00.5"), 34'200'500'000'000u);
    EXPECT_EQ(parse_time_of_day("00:00:00.000000001"), 1u);
    EXPECT_FALSE(parse_time_of_day("9:30:00").has_value());
    EXPECT_FALSE(parse_time_of_day("09:60:00").has_value());
    EXPECT_FALSE(parse_time_of_day("09:30:00.1234567890").has_value());
    EXPECT_FALSE(parse_time_of_day("09:30:00x").has_value());
}

TEST(ItchStream, CallbackReturningFalseStopsEarly) {
    std::vector<std::uint8_t> data;
    for (const auto& m : day()) {
        const auto fr = m.framed();
        data.insert(data.end(), fr.begin(), fr.end());
    }
    tickforge::test::MemorySource src(data, 7);
    int seen = 0;
    const auto st = itch::for_each_frame(src, [&](const std::uint8_t*, std::size_t) { return ++seen < 3; });
    EXPECT_TRUE(st.stopped);
    EXPECT_EQ(seen, 3);
    EXPECT_EQ(st.messages, 3u);
}
