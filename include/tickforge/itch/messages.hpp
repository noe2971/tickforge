#pragma once
// Typed structs for every Nasdaq TotalView-ITCH 5.0 message.
//
// Layouts follow the official "Nasdaq TotalView-ITCH 5.0" specification
// (sections 1.1-1.8). Every message starts with the same 11-byte header:
//
//   offset 0  len 1  Message Type   (ASCII letter)
//   offset 1  len 2  Stock Locate   (per-day integer id of the symbol; 0 = none)
//   offset 3  len 2  Tracking Number
//   offset 5  len 6  Timestamp      (nanoseconds since midnight)
//
// Field conventions from the spec's "Data Types" section:
//   * integers are unsigned, big-endian;
//   * Alpha fields are ASCII, left-justified, right-padded with spaces;
//   * Price(4) is an integer with 4 implied decimals ($1.2345 -> 12345);
//   * Price(8) is an integer with 8 implied decimals.
//
// kSize is the full message length in bytes (type byte included) and is what
// the 2-byte length prefix in the file must equal.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tickforge::itch {

using Price4 = std::uint32_t;  // 4 implied decimal places
using Price8 = std::uint64_t;  // 8 implied decimal places

inline constexpr double price4_to_double(Price4 p) noexcept { return static_cast<double>(p) / 1e4; }
inline constexpr double price8_to_double(Price8 p) noexcept { return static_cast<double>(p) / 1e8; }

// Fixed-width ASCII field (e.g. 8-char stock symbol, 4-char MPID).
template <std::size_t N>
struct Alpha {
    std::array<char, N> chars{};

    // The value with the right-padding spaces removed.
    std::string_view view() const noexcept {
        std::size_t n = N;
        while (n > 0 && chars[n - 1] == ' ') --n;
        return {chars.data(), n};
    }
    friend bool operator==(const Alpha&, const Alpha&) = default;
};

using Symbol = Alpha<8>;
using Mpid = Alpha<4>;

// Builds a space-padded Alpha from a string (truncates if too long).
template <std::size_t N>
constexpr Alpha<N> make_alpha(std::string_view s) noexcept {
    Alpha<N> a;
    for (std::size_t i = 0; i < N; ++i) a.chars[i] = i < s.size() ? s[i] : ' ';
    return a;
}
inline constexpr Symbol make_symbol(std::string_view s) noexcept { return make_alpha<8>(s); }

struct Header {
    std::uint16_t stock_locate = 0;
    std::uint16_t tracking_number = 0;
    std::uint64_t timestamp_ns = 0;  // nanoseconds since midnight (48-bit on the wire)
};
inline constexpr std::size_t kHeaderSize = 11;

// ---- 1.1 System Event ('S') ----------------------------------------------
struct SystemEvent {
    static constexpr char kType = 'S';
    static constexpr std::size_t kSize = 12;
    Header header;
    char event_code = 0;  // O, S, Q, M, E, C
};

// ---- 1.2 Stock related messages -------------------------------------------
struct StockDirectory {
    static constexpr char kType = 'R';
    static constexpr std::size_t kSize = 39;
    Header header;
    Symbol stock;
    char market_category = 0;  // Q/G/S = Nasdaq tiers, N = NYSE, A, P, Z, V
    char financial_status_indicator = 0;
    std::uint32_t round_lot_size = 0;
    char round_lots_only = 0;
    char issue_classification = 0;
    Alpha<2> issue_sub_type;
    char authenticity = 0;  // P = production, T = test
    char short_sale_threshold_indicator = 0;
    char ipo_flag = 0;
    char luld_reference_price_tier = 0;
    char etp_flag = 0;
    std::uint32_t etp_leverage_factor = 0;
    char inverse_indicator = 0;
};

struct StockTradingAction {
    static constexpr char kType = 'H';
    static constexpr std::size_t kSize = 25;
    Header header;
    Symbol stock;
    char trading_state = 0;  // H halted, P paused, Q quotation only, T trading
    char reserved = 0;
    Alpha<4> reason;
};

struct RegShoRestriction {
    static constexpr char kType = 'Y';
    static constexpr std::size_t kSize = 20;
    Header header;
    Symbol stock;
    char reg_sho_action = 0;
};

struct MarketParticipantPosition {
    static constexpr char kType = 'L';
    static constexpr std::size_t kSize = 26;
    Header header;
    Mpid mpid;
    Symbol stock;
    char primary_market_maker = 0;
    char market_maker_mode = 0;
    char market_participant_state = 0;
};

struct MwcbDeclineLevel {
    static constexpr char kType = 'V';
    static constexpr std::size_t kSize = 35;
    Header header;
    Price8 level1 = 0;
    Price8 level2 = 0;
    Price8 level3 = 0;
};

struct MwcbStatus {
    static constexpr char kType = 'W';
    static constexpr std::size_t kSize = 12;
    Header header;
    char breached_level = 0;
};

struct IpoQuotingPeriodUpdate {
    static constexpr char kType = 'K';
    static constexpr std::size_t kSize = 28;
    Header header;
    Symbol stock;
    std::uint32_t ipo_quotation_release_time = 0;  // seconds since midnight
    char ipo_quotation_release_qualifier = 0;
    Price4 ipo_price = 0;
};

struct LuldAuctionCollar {
    static constexpr char kType = 'J';
    static constexpr std::size_t kSize = 35;
    Header header;
    Symbol stock;
    Price4 auction_collar_reference_price = 0;
    Price4 upper_auction_collar_price = 0;
    Price4 lower_auction_collar_price = 0;
    std::uint32_t auction_collar_extension = 0;
};

struct OperationalHalt {
    static constexpr char kType = 'h';
    static constexpr std::size_t kSize = 21;
    Header header;
    Symbol stock;
    char market_code = 0;
    char operational_halt_action = 0;
};

// ---- 1.3 Add order --------------------------------------------------------
struct AddOrder {
    static constexpr char kType = 'A';
    static constexpr std::size_t kSize = 36;
    Header header;
    std::uint64_t order_reference_number = 0;
    char side = 0;  // 'B' buy, 'S' sell
    std::uint32_t shares = 0;
    Symbol stock;
    Price4 price = 0;
};

struct AddOrderMpid {
    static constexpr char kType = 'F';
    static constexpr std::size_t kSize = 40;
    Header header;
    std::uint64_t order_reference_number = 0;
    char side = 0;
    std::uint32_t shares = 0;
    Symbol stock;
    Price4 price = 0;
    Mpid attribution;
};

// ---- 1.4 Modify order -----------------------------------------------------
struct OrderExecuted {
    static constexpr char kType = 'E';
    static constexpr std::size_t kSize = 31;
    Header header;
    std::uint64_t order_reference_number = 0;
    std::uint32_t executed_shares = 0;
    std::uint64_t match_number = 0;
};

struct OrderExecutedWithPrice {
    static constexpr char kType = 'C';
    static constexpr std::size_t kSize = 36;
    Header header;
    std::uint64_t order_reference_number = 0;
    std::uint32_t executed_shares = 0;
    std::uint64_t match_number = 0;
    char printable = 0;  // 'Y' / 'N'
    Price4 execution_price = 0;
};

struct OrderCancel {
    static constexpr char kType = 'X';
    static constexpr std::size_t kSize = 23;
    Header header;
    std::uint64_t order_reference_number = 0;
    std::uint32_t cancelled_shares = 0;
};

struct OrderDelete {
    static constexpr char kType = 'D';
    static constexpr std::size_t kSize = 19;
    Header header;
    std::uint64_t order_reference_number = 0;
};

struct OrderReplace {
    static constexpr char kType = 'U';
    static constexpr std::size_t kSize = 35;
    Header header;
    std::uint64_t original_order_reference_number = 0;
    std::uint64_t new_order_reference_number = 0;
    std::uint32_t shares = 0;
    Price4 price = 0;
};

// ---- 1.5 Trades -----------------------------------------------------------
struct Trade {
    static constexpr char kType = 'P';
    static constexpr std::size_t kSize = 44;
    Header header;
    std::uint64_t order_reference_number = 0;
    char side = 0;
    std::uint32_t shares = 0;
    Symbol stock;
    Price4 price = 0;
    std::uint64_t match_number = 0;
};

struct CrossTrade {
    static constexpr char kType = 'Q';
    static constexpr std::size_t kSize = 40;
    Header header;
    std::uint64_t shares = 0;  // 8 bytes, unlike other share fields
    Symbol stock;
    Price4 cross_price = 0;
    std::uint64_t match_number = 0;
    char cross_type = 0;  // O open, C close, H halt/IPO, A extended close
};

struct BrokenTrade {
    static constexpr char kType = 'B';
    static constexpr std::size_t kSize = 19;
    Header header;
    std::uint64_t match_number = 0;
};

// ---- 1.6 NOII --------------------------------------------------------------
struct Noii {
    static constexpr char kType = 'I';
    static constexpr std::size_t kSize = 50;
    Header header;
    std::uint64_t paired_shares = 0;
    std::uint64_t imbalance_shares = 0;
    char imbalance_direction = 0;
    Symbol stock;
    Price4 far_price = 0;
    Price4 near_price = 0;
    Price4 current_reference_price = 0;
    char cross_type = 0;
    char price_variation_indicator = 0;
};

// ---- 1.7 RPII --------------------------------------------------------------
struct Rpii {
    static constexpr char kType = 'N';
    static constexpr std::size_t kSize = 20;
    Header header;
    Symbol stock;
    char interest_flag = 0;
};

// ---- 1.8 DLCR price discovery ---------------------------------------------
struct DlcrPriceDiscovery {
    static constexpr char kType = 'O';
    static constexpr std::size_t kSize = 48;
    Header header;
    Symbol stock;
    char open_eligibility_status = 0;
    Price4 minimum_allowable_price = 0;
    Price4 maximum_allowable_price = 0;
    Price4 near_execution_price = 0;
    std::uint64_t near_execution_time = 0;
    Price4 lower_price_range_collar = 0;
    Price4 upper_price_range_collar = 0;
};

// ---- Message-type catalogue -----------------------------------------------
struct MessageTypeInfo {
    char type;
    std::size_t size;
    std::string_view name;
};

inline constexpr std::array<MessageTypeInfo, 23> kMessageTypes{{
    {SystemEvent::kType, SystemEvent::kSize, "System Event"},
    {StockDirectory::kType, StockDirectory::kSize, "Stock Directory"},
    {StockTradingAction::kType, StockTradingAction::kSize, "Stock Trading Action"},
    {RegShoRestriction::kType, RegShoRestriction::kSize, "Reg SHO Restriction"},
    {MarketParticipantPosition::kType, MarketParticipantPosition::kSize, "Market Participant Position"},
    {MwcbDeclineLevel::kType, MwcbDeclineLevel::kSize, "MWCB Decline Level"},
    {MwcbStatus::kType, MwcbStatus::kSize, "MWCB Status"},
    {IpoQuotingPeriodUpdate::kType, IpoQuotingPeriodUpdate::kSize, "IPO Quoting Period Update"},
    {LuldAuctionCollar::kType, LuldAuctionCollar::kSize, "LULD Auction Collar"},
    {OperationalHalt::kType, OperationalHalt::kSize, "Operational Halt"},
    {AddOrder::kType, AddOrder::kSize, "Add Order"},
    {AddOrderMpid::kType, AddOrderMpid::kSize, "Add Order with MPID"},
    {OrderExecuted::kType, OrderExecuted::kSize, "Order Executed"},
    {OrderExecutedWithPrice::kType, OrderExecutedWithPrice::kSize, "Order Executed with Price"},
    {OrderCancel::kType, OrderCancel::kSize, "Order Cancel"},
    {OrderDelete::kType, OrderDelete::kSize, "Order Delete"},
    {OrderReplace::kType, OrderReplace::kSize, "Order Replace"},
    {Trade::kType, Trade::kSize, "Trade (non-cross)"},
    {CrossTrade::kType, CrossTrade::kSize, "Cross Trade"},
    {BrokenTrade::kType, BrokenTrade::kSize, "Broken Trade"},
    {Noii::kType, Noii::kSize, "NOII"},
    {Rpii::kType, Rpii::kSize, "Retail Price Improvement Indicator"},
    {DlcrPriceDiscovery::kType, DlcrPriceDiscovery::kSize, "DLCR Price Discovery"},
}};

// Index of a type letter in kMessageTypes, or -1 if it is not an ITCH 5.0 type.
constexpr int message_type_index(char type) noexcept {
    for (std::size_t i = 0; i < kMessageTypes.size(); ++i)
        if (kMessageTypes[i].type == type) return static_cast<int>(i);
    return -1;
}

}  // namespace tickforge::itch
