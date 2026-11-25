#include <gtest/gtest.h>

#include "itch_test_util.hpp"
#include "tickforge/itch/symbol_filter.hpp"

using namespace tickforge::itch;
using tickforge::test::Bytes;

namespace {
Bytes directory(std::uint16_t locate, const char* sym) {
    Bytes b;
    b.header('R', locate, 0, 1)
        .alpha(sym, 8)
        .ch('Q').ch('N').u32(100).ch('N').ch('C').alpha("Z", 2).ch('P').ch('N').ch('N').ch('1').ch('N').u32(0).ch('N');
    return b;
}
Bytes add(std::uint16_t locate, const char* sym) {
    Bytes b;
    b.header('A', locate, 0, 2).u64(1).ch('B').u32(100).alpha(sym, 8).u32(10'000);
    return b;
}
bool keep(SymbolFilter& f, const Bytes& b) { return f.keep(b.b.data(), b.b.size()); }
}  // namespace

TEST(SymbolFilter, KeepsChosenSymbolsAndMarketWideMessages) {
    SymbolFilter f({"AAPL", "INTC"});
    EXPECT_TRUE(keep(f, Bytes().header('S', 0, 0, 0).ch('O')));  // system event
    EXPECT_TRUE(keep(f, directory(10, "AAPL")));
    EXPECT_FALSE(keep(f, directory(11, "MSFT")));
    EXPECT_TRUE(keep(f, directory(12, "INTC")));
    EXPECT_TRUE(keep(f, add(10, "AAPL")));
    EXPECT_FALSE(keep(f, add(11, "MSFT")));
    EXPECT_TRUE(keep(f, add(12, "INTC")));
    // Order Delete has no symbol: filtered by locate alone.
    EXPECT_TRUE(keep(f, Bytes().header('D', 10, 0, 3).u64(1)));
    EXPECT_FALSE(keep(f, Bytes().header('D', 11, 0, 3).u64(1)));
    // MWCB messages are market-wide.
    EXPECT_TRUE(keep(f, Bytes().header('W', 0, 0, 4).ch('1')));
    // IPO quoting update uses locate 0, so it is filtered by its symbol.
    EXPECT_TRUE(keep(f, Bytes().header('K', 0, 0, 5).alpha("AAPL", 8).u32(0).ch('A').u32(0)));
    EXPECT_FALSE(keep(f, Bytes().header('K', 0, 0, 5).alpha("NEWCO", 8).u32(0).ch('A').u32(0)));

    ASSERT_EQ(f.found().size(), 2u);
    EXPECT_EQ(f.found()[0].symbol, "AAPL");
    EXPECT_EQ(f.found()[0].locate, 10);
    EXPECT_EQ(f.found()[1].symbol, "INTC");
    EXPECT_EQ(f.found()[1].locate, 12);
}

TEST(SymbolFilter, MessagesBeforeTheDirectoryEntryAreDropped) {
    SymbolFilter f({"AAPL"});
    EXPECT_FALSE(keep(f, add(10, "AAPL")));  // locate not known yet
    EXPECT_TRUE(keep(f, directory(10, "AAPL")));
    EXPECT_TRUE(keep(f, add(10, "AAPL")));
}
