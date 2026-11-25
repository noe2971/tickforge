// Every ITCH 5.0 message type: a hand-built byte string (field order, offsets
// and widths from the Nasdaq TotalView-ITCH 5.0 spec) must decode to exactly
// the fields that were written, and be routed by dispatch() to the right type.

#include <gtest/gtest.h>

#include "itch_test_util.hpp"
#include "tickforge/itch/decode.hpp"

using namespace tickforge::itch;
using tickforge::test::Bytes;
using tickforge::test::Capture;

namespace {

// Header values used throughout. The timestamp needs all 48 bits to catch a
// decoder that drops the top 16: 15:59:59.999999999 = 57,599,999,999,999 ns
// = 0x3463_0B89_FFFF, which doesn't fit in 32 bits.
constexpr std::uint16_t kLocate = 0x1234;
constexpr std::uint16_t kTracking = 0xBEEF;
constexpr std::uint64_t kTs = 57'599'999'999'999ULL;

template <typename M>
M decode_via_dispatch(const Bytes& msg) {
    EXPECT_EQ(msg.b.size(), M::kSize) << "test message length disagrees with the spec";
    Capture cap;
    EXPECT_EQ(dispatch(msg.b.data(), msg.b.size(), cap), DecodeStatus::ok);
    EXPECT_EQ(cap.messages.size(), 1u);
    if (cap.messages.size() != 1 || !std::holds_alternative<M>(cap.messages[0])) {
        ADD_FAILURE() << "dispatch routed to the wrong message type";
        return M{};
    }
    return std::get<M>(cap.messages[0]);
}

void expect_header(const Header& h, std::uint16_t locate = kLocate) {
    EXPECT_EQ(h.stock_locate, locate);
    EXPECT_EQ(h.tracking_number, kTracking);
    EXPECT_EQ(h.timestamp_ns, kTs);
}

}  // namespace

// A fully literal byte string (no builder) to anchor the builder itself.
TEST(ItchDecode, AddOrderFromLiteralBytes) {
    const std::uint8_t raw[] = {
        0x41,                                            // 'A'
        0x00, 0x01,                                      // stock locate 1
        0x00, 0x02,                                      // tracking number 2
        0x00, 0x00, 0x12, 0x34, 0x56, 0x78,              // timestamp 0x12345678 ns
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39,  // order ref 12345
        0x42,                                            // 'B'
        0x00, 0x00, 0x00, 0x64,                          // 100 shares
        0x41, 0x41, 0x50, 0x4C, 0x20, 0x20, 0x20, 0x20,  // "AAPL    "
        0x00, 0x17, 0xD7, 0x84,                          // price 1562500 = $156.2500
    };
    static_assert(sizeof raw == AddOrder::kSize);
    const AddOrder m = decode<AddOrder>(raw);
    EXPECT_EQ(m.header.stock_locate, 1);
    EXPECT_EQ(m.header.tracking_number, 2);
    EXPECT_EQ(m.header.timestamp_ns, 0x12345678u);
    EXPECT_EQ(m.order_reference_number, 12345u);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 100u);
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.price, 1'562'500u);
    EXPECT_DOUBLE_EQ(price4_to_double(m.price), 156.25);
}

TEST(ItchDecode, SystemEvent) {
    Bytes b;
    b.header('S', 0, kTracking, kTs).ch('Q');
    const auto m = decode_via_dispatch<SystemEvent>(b);
    expect_header(m.header, 0);
    EXPECT_EQ(m.event_code, 'Q');
}

TEST(ItchDecode, StockDirectory) {
    Bytes b;
    b.header('R', kLocate, kTracking, kTs)
        .alpha("MSFT", 8)
        .ch('Q')         // market category
        .ch('N')         // financial status
        .u32(100)        // round lot size
        .ch('N')         // round lots only
        .ch('C')         // issue classification
        .alpha("Z", 2)   // issue sub-type
        .ch('P')         // authenticity
        .ch('N')         // short sale threshold
        .ch('N')         // IPO flag
        .ch('1')         // LULD tier
        .ch('N')         // ETP flag
        .u32(0x01020304) // ETP leverage factor
        .ch('N');        // inverse indicator
    const auto m = decode_via_dispatch<StockDirectory>(b);
    expect_header(m.header);
    EXPECT_EQ(m.stock.view(), "MSFT");
    EXPECT_EQ(m.market_category, 'Q');
    EXPECT_EQ(m.financial_status_indicator, 'N');
    EXPECT_EQ(m.round_lot_size, 100u);
    EXPECT_EQ(m.round_lots_only, 'N');
    EXPECT_EQ(m.issue_classification, 'C');
    EXPECT_EQ(m.issue_sub_type.view(), "Z");
    EXPECT_EQ(m.authenticity, 'P');
    EXPECT_EQ(m.short_sale_threshold_indicator, 'N');
    EXPECT_EQ(m.ipo_flag, 'N');
    EXPECT_EQ(m.luld_reference_price_tier, '1');
    EXPECT_EQ(m.etp_flag, 'N');
    EXPECT_EQ(m.etp_leverage_factor, 0x01020304u);
    EXPECT_EQ(m.inverse_indicator, 'N');
}

TEST(ItchDecode, StockTradingAction) {
    Bytes b;
    b.header('H', kLocate, kTracking, kTs).alpha("INTC", 8).ch('H').ch(' ').alpha("LUDP", 4);
    const auto m = decode_via_dispatch<StockTradingAction>(b);
    expect_header(m.header);
    EXPECT_EQ(m.stock.view(), "INTC");
    EXPECT_EQ(m.trading_state, 'H');
    EXPECT_EQ(m.reserved, ' ');
    EXPECT_EQ(m.reason.view(), "LUDP");
}

TEST(ItchDecode, RegShoRestriction) {
    Bytes b;
    b.header('Y', kLocate, kTracking, kTs).alpha("AAPL", 8).ch('1');
    const auto m = decode_via_dispatch<RegShoRestriction>(b);
    expect_header(m.header);
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.reg_sho_action, '1');
}

TEST(ItchDecode, MarketParticipantPosition) {
    Bytes b;
    b.header('L', kLocate, kTracking, kTs).alpha("GSCO", 4).alpha("AAPL", 8).ch('Y').ch('N').ch('A');
    const auto m = decode_via_dispatch<MarketParticipantPosition>(b);
    expect_header(m.header);
    EXPECT_EQ(m.mpid.view(), "GSCO");
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.primary_market_maker, 'Y');
    EXPECT_EQ(m.market_maker_mode, 'N');
    EXPECT_EQ(m.market_participant_state, 'A');
}

TEST(ItchDecode, MwcbDeclineLevel) {
    // Price(8): 8 implied decimals. 2,345.67000000 -> 234,567,000,000.
    Bytes b;
    b.header('V', 0, kTracking, kTs).u64(234'567'000'000ULL).u64(0x0102030405060708ULL).u64(0xFFFFFFFFFFFFFFFFULL);
    const auto m = decode_via_dispatch<MwcbDeclineLevel>(b);
    expect_header(m.header, 0);
    EXPECT_EQ(m.level1, 234'567'000'000ULL);
    EXPECT_DOUBLE_EQ(price8_to_double(m.level1), 2345.67);
    EXPECT_EQ(m.level2, 0x0102030405060708ULL);
    EXPECT_EQ(m.level3, 0xFFFFFFFFFFFFFFFFULL);
}

TEST(ItchDecode, MwcbStatus) {
    Bytes b;
    b.header('W', 0, kTracking, kTs).ch('2');
    const auto m = decode_via_dispatch<MwcbStatus>(b);
    expect_header(m.header, 0);
    EXPECT_EQ(m.breached_level, '2');
}

TEST(ItchDecode, IpoQuotingPeriodUpdate) {
    Bytes b;
    b.header('K', 0, kTracking, kTs).alpha("NEWCO", 8).u32(34'200).ch('A').u32(150'000);
    const auto m = decode_via_dispatch<IpoQuotingPeriodUpdate>(b);
    expect_header(m.header, 0);
    EXPECT_EQ(m.stock.view(), "NEWCO");
    EXPECT_EQ(m.ipo_quotation_release_time, 34'200u);
    EXPECT_EQ(m.ipo_quotation_release_qualifier, 'A');
    EXPECT_EQ(m.ipo_price, 150'000u);
}

TEST(ItchDecode, LuldAuctionCollar) {
    Bytes b;
    b.header('J', kLocate, kTracking, kTs).alpha("AAPL", 8).u32(1'500'000).u32(1'575'000).u32(1'425'000).u32(3);
    const auto m = decode_via_dispatch<LuldAuctionCollar>(b);
    expect_header(m.header);
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.auction_collar_reference_price, 1'500'000u);
    EXPECT_EQ(m.upper_auction_collar_price, 1'575'000u);
    EXPECT_EQ(m.lower_auction_collar_price, 1'425'000u);
    EXPECT_EQ(m.auction_collar_extension, 3u);
}

TEST(ItchDecode, OperationalHalt) {
    Bytes b;
    b.header('h', kLocate, kTracking, kTs).alpha("AAPL", 8).ch('Q').ch('H');
    const auto m = decode_via_dispatch<OperationalHalt>(b);
    expect_header(m.header);
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.market_code, 'Q');
    EXPECT_EQ(m.operational_halt_action, 'H');
}

TEST(ItchDecode, AddOrder) {
    Bytes b;
    b.header('A', kLocate, kTracking, kTs).u64(0x00FFEEDDCCBBAA99ULL).ch('S').u32(250).alpha("MSFT", 8).u32(1'049'900);
    const auto m = decode_via_dispatch<AddOrder>(b);
    expect_header(m.header);
    EXPECT_EQ(m.order_reference_number, 0x00FFEEDDCCBBAA99ULL);
    EXPECT_EQ(m.side, 'S');
    EXPECT_EQ(m.shares, 250u);
    EXPECT_EQ(m.stock.view(), "MSFT");
    EXPECT_EQ(m.price, 1'049'900u);  // $104.99
}

TEST(ItchDecode, AddOrderMpid) {
    Bytes b;
    b.header('F', kLocate, kTracking, kTs).u64(987'654'321).ch('B').u32(1'000).alpha("INTC", 8).u32(487'500).alpha(
        "VIRT", 4);
    const auto m = decode_via_dispatch<AddOrderMpid>(b);
    expect_header(m.header);
    EXPECT_EQ(m.order_reference_number, 987'654'321u);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 1'000u);
    EXPECT_EQ(m.stock.view(), "INTC");
    EXPECT_EQ(m.price, 487'500u);
    EXPECT_EQ(m.attribution.view(), "VIRT");
}

TEST(ItchDecode, OrderExecuted) {
    Bytes b;
    b.header('E', kLocate, kTracking, kTs).u64(42).u32(0x80000001u).u64(0xDEADBEEFCAFEF00DULL);
    const auto m = decode_via_dispatch<OrderExecuted>(b);
    expect_header(m.header);
    EXPECT_EQ(m.order_reference_number, 42u);
    EXPECT_EQ(m.executed_shares, 0x80000001u);  // high bit set: must stay unsigned
    EXPECT_EQ(m.match_number, 0xDEADBEEFCAFEF00DULL);
}

TEST(ItchDecode, OrderExecutedWithPrice) {
    Bytes b;
    b.header('C', kLocate, kTracking, kTs).u64(43).u32(75).u64(1'001).ch('Y').u32(1'562'600);
    const auto m = decode_via_dispatch<OrderExecutedWithPrice>(b);
    expect_header(m.header);
    EXPECT_EQ(m.order_reference_number, 43u);
    EXPECT_EQ(m.executed_shares, 75u);
    EXPECT_EQ(m.match_number, 1'001u);
    EXPECT_EQ(m.printable, 'Y');
    EXPECT_EQ(m.execution_price, 1'562'600u);
}

TEST(ItchDecode, OrderCancel) {
    Bytes b;
    b.header('X', kLocate, kTracking, kTs).u64(44).u32(30);
    const auto m = decode_via_dispatch<OrderCancel>(b);
    expect_header(m.header);
    EXPECT_EQ(m.order_reference_number, 44u);
    EXPECT_EQ(m.cancelled_shares, 30u);
}

TEST(ItchDecode, OrderDelete) {
    Bytes b;
    b.header('D', kLocate, kTracking, kTs).u64(0xFFFFFFFFFFFFFFFFULL);
    const auto m = decode_via_dispatch<OrderDelete>(b);
    expect_header(m.header);
    EXPECT_EQ(m.order_reference_number, 0xFFFFFFFFFFFFFFFFULL);
}

TEST(ItchDecode, OrderReplace) {
    Bytes b;
    b.header('U', kLocate, kTracking, kTs).u64(45).u64(46).u32(500).u32(1'563'000);
    const auto m = decode_via_dispatch<OrderReplace>(b);
    expect_header(m.header);
    EXPECT_EQ(m.original_order_reference_number, 45u);
    EXPECT_EQ(m.new_order_reference_number, 46u);
    EXPECT_EQ(m.shares, 500u);
    EXPECT_EQ(m.price, 1'563'000u);
}

TEST(ItchDecode, Trade) {
    Bytes b;
    b.header('P', kLocate, kTracking, kTs).u64(0).ch('B').u32(300).alpha("AAPL", 8).u32(1'562'550).u64(2'002);
    const auto m = decode_via_dispatch<Trade>(b);
    expect_header(m.header);
    EXPECT_EQ(m.order_reference_number, 0u);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 300u);
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.price, 1'562'550u);
    EXPECT_EQ(m.match_number, 2'002u);
}

TEST(ItchDecode, CrossTrade) {
    // Cross shares are 8 bytes wide; use a value that needs more than 32 bits.
    Bytes b;
    b.header('Q', kLocate, kTracking, kTs).u64(5'000'000'000ULL).alpha("AAPL", 8).u32(1'655'000).u64(3'003).ch('C');
    const auto m = decode_via_dispatch<CrossTrade>(b);
    expect_header(m.header);
    EXPECT_EQ(m.shares, 5'000'000'000ULL);
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.cross_price, 1'655'000u);
    EXPECT_EQ(m.match_number, 3'003u);
    EXPECT_EQ(m.cross_type, 'C');
}

TEST(ItchDecode, BrokenTrade) {
    Bytes b;
    b.header('B', kLocate, kTracking, kTs).u64(3'003);
    const auto m = decode_via_dispatch<BrokenTrade>(b);
    expect_header(m.header);
    EXPECT_EQ(m.match_number, 3'003u);
}

TEST(ItchDecode, Noii) {
    Bytes b;
    b.header('I', kLocate, kTracking, kTs)
        .u64(1'000'000)
        .u64(25'000)
        .ch('B')
        .alpha("AAPL", 8)
        .u32(1'660'000)
        .u32(1'658'000)
        .u32(1'657'500)
        .ch('C')
        .ch('L');
    const auto m = decode_via_dispatch<Noii>(b);
    expect_header(m.header);
    EXPECT_EQ(m.paired_shares, 1'000'000u);
    EXPECT_EQ(m.imbalance_shares, 25'000u);
    EXPECT_EQ(m.imbalance_direction, 'B');
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.far_price, 1'660'000u);
    EXPECT_EQ(m.near_price, 1'658'000u);
    EXPECT_EQ(m.current_reference_price, 1'657'500u);
    EXPECT_EQ(m.cross_type, 'C');
    EXPECT_EQ(m.price_variation_indicator, 'L');
}

TEST(ItchDecode, Rpii) {
    Bytes b;
    b.header('N', kLocate, kTracking, kTs).alpha("AAPL", 8).ch('A');
    const auto m = decode_via_dispatch<Rpii>(b);
    expect_header(m.header);
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.interest_flag, 'A');
}

TEST(ItchDecode, DlcrPriceDiscovery) {
    Bytes b;
    b.header('O', kLocate, kTracking, kTs)
        .alpha("DLCR", 8)
        .ch('Y')
        .u32(80'000)
        .u32(180'000)
        .u32(105'000)
        .u64(36'000'000'000'000ULL)
        .u32(95'000)
        .u32(115'000);
    const auto m = decode_via_dispatch<DlcrPriceDiscovery>(b);
    expect_header(m.header);
    EXPECT_EQ(m.stock.view(), "DLCR");
    EXPECT_EQ(m.open_eligibility_status, 'Y');
    EXPECT_EQ(m.minimum_allowable_price, 80'000u);
    EXPECT_EQ(m.maximum_allowable_price, 180'000u);
    EXPECT_EQ(m.near_execution_price, 105'000u);
    EXPECT_EQ(m.near_execution_time, 36'000'000'000'000ULL);
    EXPECT_EQ(m.lower_price_range_collar, 95'000u);
    EXPECT_EQ(m.upper_price_range_collar, 115'000u);
}

// ---- Error handling and catalogue -------------------------------------------

TEST(ItchDecode, UnknownTypeIsReportedNotDecoded) {
    Bytes b;
    b.header('Z', kLocate, kTracking, kTs).u32(0);
    Capture cap;
    EXPECT_EQ(dispatch(b.b.data(), b.b.size(), cap), DecodeStatus::unknown_type);
    EXPECT_EQ(cap.unknown, 1);
    EXPECT_TRUE(cap.messages.empty());
}

TEST(ItchDecode, WrongLengthIsReportedNotDecoded) {
    Bytes b;
    b.header('D', kLocate, kTracking, kTs).u64(1).u8(0);  // one byte too long
    Capture cap;
    EXPECT_EQ(dispatch(b.b.data(), b.b.size(), cap), DecodeStatus::bad_length);
    EXPECT_EQ(dispatch(b.b.data(), 0, cap), DecodeStatus::bad_length);
    EXPECT_EQ(cap.bad_length, 2);
    EXPECT_TRUE(cap.messages.empty());
}

TEST(ItchDecode, CatalogueHasAll23TypesWithUniqueLetters) {
    EXPECT_EQ(kMessageTypes.size(), 23u);
    for (std::size_t i = 0; i < kMessageTypes.size(); ++i) {
        EXPECT_EQ(message_type_index(kMessageTypes[i].type), static_cast<int>(i));
    }
    EXPECT_EQ(message_type_index('Z'), -1);
}

TEST(ItchDecode, AlphaTrimsOnlyTrailingSpaces) {
    EXPECT_EQ(make_symbol("BRK A").view(), "BRK A");
    EXPECT_EQ(make_symbol("").view(), "");
    EXPECT_EQ(make_symbol("ABCDEFGHIJ").view(), "ABCDEFGH");
}
