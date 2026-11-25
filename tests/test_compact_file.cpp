// Round trip: messages written with CompactWriter must come back from
// CompactReader byte-for-byte, in order, and decode to the same fields.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "itch_test_util.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/stream.hpp"

using namespace tickforge;
using namespace tickforge::itch;
using tickforge::test::Bytes;
using tickforge::test::Capture;
using tickforge::test::TempFile;

namespace {
std::vector<Bytes> sample_messages() {
    std::vector<Bytes> v;
    v.push_back(Bytes().header('S', 0, 1, 100).ch('O'));
    v.push_back(Bytes().header('A', 10, 2, 200).u64(77).ch('S').u32(300).alpha("AAPL", 8).u32(1'562'500));
    v.push_back(Bytes().header('F', 12, 3, 300).u64(78).ch('B').u32(10).alpha("INTC", 8).u32(487'500).alpha("GSCO", 4));
    v.push_back(Bytes().header('U', 10, 4, 400).u64(77).u64(79).u32(200).u32(1'562'600));
    v.push_back(Bytes().header('C', 10, 5, 57'599'999'999'999ULL).u64(79).u32(200).u64(5).ch('Y').u32(1'562'600));
    v.push_back(Bytes().header('Q', 10, 6, 57'600'000'000'000ULL).u64(5'000'000'000ULL).alpha("AAPL", 8).u32(1'655'000).u64(6).ch('C'));
    return v;
}
}  // namespace

TEST(CompactFile, RoundTripPreservesHeaderBytesAndFields) {
    TempFile tmp("roundtrip.tfc");
    const auto msgs = sample_messages();
    {
        CompactWriter w(tmp.path, "01302019.NASDAQ_ITCH50.gz", {"AAPL", "INTC", "NOPE"});
        for (const auto& m : msgs) w.write(m.b.data(), m.b.size());
        w.set_locate("AAPL", 10);
        w.set_locate("INTC", 12);
        w.close();
    }

    CompactReader r(tmp.path);
    const CompactHeader& h = r.header();
    EXPECT_EQ(h.version, kCompactVersion);
    EXPECT_EQ(h.source, "01302019.NASDAQ_ITCH50.gz");
    ASSERT_EQ(h.symbols.size(), 3u);
    EXPECT_EQ(h.symbols[0], (CompactSymbol{10, make_symbol("AAPL")}));
    EXPECT_EQ(h.symbols[1], (CompactSymbol{12, make_symbol("INTC")}));
    EXPECT_EQ(h.symbols[2], (CompactSymbol{0, make_symbol("NOPE")}));  // requested, not found
    EXPECT_EQ(h.message_count, msgs.size());

    // Byte-for-byte.
    std::vector<std::vector<std::uint8_t>> got;
    const auto st = for_each_frame(r, [&](const std::uint8_t* p, std::size_t n) { got.emplace_back(p, p + n); });
    ASSERT_EQ(got.size(), msgs.size());
    for (std::size_t i = 0; i < msgs.size(); ++i) EXPECT_EQ(got[i], msgs[i].b) << "message " << i;
    EXPECT_EQ(st.bytes, h.payload_bytes);

    // Decoded fields.
    CompactReader r2(tmp.path);
    Capture cap;
    parse_stream(r2, cap);
    ASSERT_EQ(cap.messages.size(), msgs.size());
    const auto& c = std::get<OrderExecutedWithPrice>(cap.messages[4]);
    EXPECT_EQ(c.header.timestamp_ns, 57'599'999'999'999ULL);
    EXPECT_EQ(c.execution_price, 1'562'600u);
    EXPECT_EQ(std::get<CrossTrade>(cap.messages[5]).shares, 5'000'000'000ULL);
    EXPECT_EQ(std::get<AddOrderMpid>(cap.messages[2]).attribution.view(), "GSCO");
}

TEST(CompactFile, EmptyFileRoundTrips) {
    TempFile tmp("empty.tfc");
    { CompactWriter w(tmp.path, "", {}); }  // destructor closes
    CompactReader r(tmp.path);
    EXPECT_EQ(r.header().message_count, 0u);
    EXPECT_TRUE(r.header().symbols.empty());
    Capture cap;
    EXPECT_EQ(parse_stream(r, cap).messages, 0u);
}

TEST(CompactFile, TruncatedFileIsRejected) {
    TempFile tmp("trunc.tfc");
    {
        CompactWriter w(tmp.path, "src", {"AAPL"});
        for (const auto& m : sample_messages()) w.write(m.b.data(), m.b.size());
    }
    std::filesystem::resize_file(tmp.path, std::filesystem::file_size(tmp.path) - 3);
    EXPECT_THROW(CompactReader{tmp.path}, std::runtime_error);
}

TEST(CompactFile, WrongMagicIsRejected) {
    TempFile tmp("bad.tfc");
    {
        std::ofstream f(tmp.path, std::ios::binary);
        f << std::string(64, 'x');
    }
    EXPECT_THROW(CompactReader{tmp.path}, std::runtime_error);
}
