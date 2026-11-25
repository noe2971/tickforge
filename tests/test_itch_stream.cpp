// The streaming framer must deliver every message exactly once and in order,
// however the byte stream is chunked.

#include <gtest/gtest.h>

#include "itch_test_util.hpp"
#include "tickforge/itch/stream.hpp"

using namespace tickforge;
using namespace tickforge::itch;
using tickforge::test::Bytes;
using tickforge::test::Capture;
using tickforge::test::MemorySource;

namespace {

// A small "day": system event, directory, add, execute, delete.
std::vector<std::uint8_t> sample_stream() {
    std::vector<std::uint8_t> out;
    auto append = [&](const Bytes& b) {
        auto f = b.framed();
        out.insert(out.end(), f.begin(), f.end());
    };
    append(Bytes().header('S', 0, 1, 1000).ch('O'));
    append(Bytes()
               .header('R', 7, 2, 2000)
               .alpha("AAPL", 8)
               .ch('Q').ch('N').u32(100).ch('N').ch('C').alpha("Z", 2).ch('P').ch('N').ch('N').ch('1').ch('N')
               .u32(0).ch('N'));
    append(Bytes().header('A', 7, 3, 3000).u64(1).ch('B').u32(100).alpha("AAPL", 8).u32(1'500'000));
    append(Bytes().header('E', 7, 4, 4000).u64(1).u32(40).u64(99));
    append(Bytes().header('D', 7, 5, 5000).u64(1));
    return out;
}

}  // namespace

TEST(ItchStream, DeliversAllMessagesInOrderForEveryChunkSize) {
    const auto data = sample_stream();
    for (std::size_t chunk : {1u, 2u, 3u, 7u, 13u, 64u, 100000u}) {
        MemorySource src(data, chunk);
        Capture cap;
        const auto st = parse_stream(src, cap);
        ASSERT_EQ(st.messages, 5u) << "chunk=" << chunk;
        EXPECT_EQ(st.bytes, data.size());
        ASSERT_EQ(cap.messages.size(), 5u);
        EXPECT_TRUE(std::holds_alternative<SystemEvent>(cap.messages[0]));
        EXPECT_EQ(std::get<StockDirectory>(cap.messages[1]).stock.view(), "AAPL");
        EXPECT_EQ(std::get<AddOrder>(cap.messages[2]).price, 1'500'000u);
        EXPECT_EQ(std::get<OrderExecuted>(cap.messages[3]).executed_shares, 40u);
        EXPECT_EQ(std::get<OrderDelete>(cap.messages[4]).header.timestamp_ns, 5000u);
    }
}

TEST(ItchStream, SmallBufferStillWorksAcrossManyRefills) {
    // Repeat the sample many times so the 64 KiB minimum buffer refills often.
    std::vector<std::uint8_t> data;
    const auto one = sample_stream();
    for (int i = 0; i < 20000; ++i) data.insert(data.end(), one.begin(), one.end());
    MemorySource src(data, 4096);
    Capture cap;
    const auto st = parse_stream(src, cap, /*buffer_size=*/1);  // rounded up to kMinFrameBuffer
    EXPECT_EQ(st.messages, 100000u);
    EXPECT_EQ(st.bytes, data.size());
    EXPECT_EQ(cap.messages.size(), 100000u);
}

TEST(ItchStream, TruncatedStreamThrows) {
    auto data = sample_stream();
    data.pop_back();  // cut the last message short
    MemorySource src(data, 1 << 20);
    Capture cap;
    EXPECT_THROW(parse_stream(src, cap), std::runtime_error);
}

TEST(ItchStream, CountsUnknownAndBadLengthWithoutStopping) {
    std::vector<std::uint8_t> data;
    auto append = [&](const Bytes& b) {
        auto f = b.framed();
        data.insert(data.end(), f.begin(), f.end());
    };
    append(Bytes().header('Z', 1, 0, 0).u8(0));        // unknown type
    append(Bytes().header('D', 1, 0, 0).u32(0));       // too short for 'D'
    append(Bytes().header('D', 1, 0, 0).u64(5));       // fine
    MemorySource src(data, 5);
    Capture cap;
    const auto st = parse_stream(src, cap);
    EXPECT_EQ(st.messages, 3u);
    EXPECT_EQ(st.unknown_type, 1u);
    EXPECT_EQ(st.bad_length, 1u);
    ASSERT_EQ(cap.messages.size(), 1u);
    EXPECT_EQ(std::get<OrderDelete>(cap.messages[0]).order_reference_number, 5u);
}

TEST(ItchStream, EmptyStreamHasNoMessages) {
    MemorySource src({}, 10);
    Capture cap;
    const auto st = parse_stream(src, cap);
    EXPECT_EQ(st.messages, 0u);
    EXPECT_EQ(st.bytes, 0u);
}
