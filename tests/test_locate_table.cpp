#include <gtest/gtest.h>

#include "tickforge/itch/locate_table.hpp"

using namespace tickforge::itch;

namespace {
StockDirectory dir(std::uint16_t locate, const char* sym) {
    StockDirectory d;
    d.header.stock_locate = locate;
    d.stock = make_symbol(sym);
    d.market_category = 'Q';
    return d;
}
}  // namespace

TEST(LocateTable, MapsBothWays) {
    LocateTable t;
    EXPECT_FALSE(t.known(13));
    EXPECT_EQ(t.symbol(13), "");
    t.add(dir(13, "AAPL"));
    t.add(dir(65535, "ZZZZ"));
    EXPECT_TRUE(t.known(13));
    EXPECT_EQ(t.symbol(13), "AAPL");
    EXPECT_EQ(t.symbol(65535), "ZZZZ");
    EXPECT_EQ(t.find("AAPL"), std::optional<std::uint16_t>(13));
    EXPECT_EQ(t.find("MSFT"), std::nullopt);
    EXPECT_EQ(t.directory(13)->market_category, 'Q');
    EXPECT_EQ(t.size(), 2u);
}

TEST(LocateTable, RepeatedDirectoryMessageDoesNotDoubleCount) {
    LocateTable t;
    t.add(dir(5, "INTC"));
    t.add(dir(5, "INTC"));
    EXPECT_EQ(t.size(), 1u);
}
