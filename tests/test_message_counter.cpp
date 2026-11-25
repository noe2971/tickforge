#include <gtest/gtest.h>

#include <sstream>

#include "itch_test_util.hpp"
#include "tickforge/itch/message_counter.hpp"
#include "tickforge/itch/stream.hpp"

using namespace tickforge::itch;
using tickforge::test::Bytes;
using tickforge::test::MemorySource;

namespace {
std::vector<std::uint8_t> day() {
    std::vector<std::uint8_t> out;
    auto append = [&](const Bytes& b) {
        auto f = b.framed();
        out.insert(out.end(), f.begin(), f.end());
    };
    auto dir = [](std::uint16_t loc, const char* sym) {
        return Bytes()
            .header('R', loc, 0, 10)
            .alpha(sym, 8)
            .ch('Q').ch('N').u32(100).ch('N').ch('C').alpha("Z", 2).ch('P').ch('N').ch('N').ch('1').ch('N').u32(0).ch('N');
    };
    append(Bytes().header('S', 0, 0, 5).ch('O'));
    append(dir(1, "AAPL"));
    append(dir(2, "MSFT"));
    for (int i = 0; i < 3; ++i) append(Bytes().header('A', 1, 0, 20).u64(i).ch('B').u32(100).alpha("AAPL", 8).u32(1));
    append(Bytes().header('A', 2, 0, 30).u64(9).ch('S').u32(100).alpha("MSFT", 8).u32(1));
    append(Bytes().header('E', 1, 0, 40).u64(0).u32(100).u64(1));
    append(Bytes().header('Q', 1, 0, 57'600'000'000'000ULL).u64(1000).alpha("AAPL", 8).u32(1'655'000).u64(2).ch('C'));
    return out;
}
}  // namespace

TEST(MessageCounter, CountsByTypeAndBySymbol) {
    MemorySource src(day(), 17);
    MessageCounter mc;
    parse_stream(src, mc);
    auto idx = [](char t) { return static_cast<std::size_t>(message_type_index(t)); };

    EXPECT_EQ(mc.total(), 9u);
    EXPECT_EQ(mc.by_type()[idx('A')], 4u);
    EXPECT_EQ(mc.by_type()[idx('R')], 2u);
    EXPECT_EQ(mc.for_locate(1)[idx('A')], 3u);
    EXPECT_EQ(mc.for_locate(2)[idx('A')], 1u);
    EXPECT_EQ(mc.for_locate(1)[idx('E')], 1u);
    EXPECT_EQ(mc.for_locate(0)[idx('S')], 1u);
    EXPECT_EQ(mc.locates().symbol(2), "MSFT");
    EXPECT_EQ(mc.closing_cross_price(1), 1'655'000u);
    EXPECT_EQ(mc.closing_cross_price(2), MessageCounter::kNoPrice);
    EXPECT_EQ(mc.first_timestamp_ns(), 5u);
    EXPECT_EQ(mc.last_timestamp_ns(), 57'600'000'000'000ULL);

    std::ostringstream by_type;
    mc.write_by_type_csv(by_type);
    EXPECT_NE(by_type.str().find("A,Add Order,36,4,44.4444"), std::string::npos) << by_type.str();

    std::ostringstream by_sym;
    mc.write_by_symbol_csv(by_sym);
    // AAPL: directory + 3 adds + 1 exec + 1 cross = 6.
    EXPECT_NE(by_sym.str().find("\n1,AAPL,Q,C,N,P,100,165.5000,6,"), std::string::npos) << by_sym.str();
}

TEST(MessageCounter, FormatsTimestamps) {
    EXPECT_EQ(format_timestamp(0), "00:00:00.000000000");
    EXPECT_EQ(format_timestamp(34'200'000'000'001ULL), "09:30:00.000000001");
    EXPECT_EQ(format_timestamp(57'599'999'999'999ULL), "15:59:59.999999999");
}
