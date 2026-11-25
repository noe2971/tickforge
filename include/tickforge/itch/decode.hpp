#pragma once
// Decoding raw ITCH 5.0 message bytes into the typed structs in messages.hpp,
// and dispatching each decoded message to a handler.
//
// `p` always points at the message type byte (offset 0 in the spec tables),
// i.e. just after the 2-byte length prefix used in the binary file format.

#include <cstddef>
#include <cstdint>

#include "tickforge/itch/endian.hpp"
#include "tickforge/itch/messages.hpp"

namespace tickforge::itch {

namespace detail {
template <std::size_t N>
inline Alpha<N> load_alpha(const std::uint8_t* p) noexcept {
    Alpha<N> a;
    for (std::size_t i = 0; i < N; ++i) a.chars[i] = static_cast<char>(p[i]);
    return a;
}
inline char ch(const std::uint8_t* p) noexcept { return static_cast<char>(*p); }
}  // namespace detail

inline Header decode_header(const std::uint8_t* p) noexcept {
    return Header{load_be16(p + 1), load_be16(p + 3), load_be48(p + 5)};
}

// decode<M>(p): decode one message of type M. The caller guarantees that the
// buffer holds at least M::kSize bytes and that p[0] == M::kType.
template <typename M>
M decode(const std::uint8_t* p) noexcept;

template <>
inline SystemEvent decode<SystemEvent>(const std::uint8_t* p) noexcept {
    return {decode_header(p), detail::ch(p + 11)};
}

template <>
inline StockDirectory decode<StockDirectory>(const std::uint8_t* p) noexcept {
    StockDirectory m;
    m.header = decode_header(p);
    m.stock = detail::load_alpha<8>(p + 11);
    m.market_category = detail::ch(p + 19);
    m.financial_status_indicator = detail::ch(p + 20);
    m.round_lot_size = load_be32(p + 21);
    m.round_lots_only = detail::ch(p + 25);
    m.issue_classification = detail::ch(p + 26);
    m.issue_sub_type = detail::load_alpha<2>(p + 27);
    m.authenticity = detail::ch(p + 29);
    m.short_sale_threshold_indicator = detail::ch(p + 30);
    m.ipo_flag = detail::ch(p + 31);
    m.luld_reference_price_tier = detail::ch(p + 32);
    m.etp_flag = detail::ch(p + 33);
    m.etp_leverage_factor = load_be32(p + 34);
    m.inverse_indicator = detail::ch(p + 38);
    return m;
}

template <>
inline StockTradingAction decode<StockTradingAction>(const std::uint8_t* p) noexcept {
    return {decode_header(p), detail::load_alpha<8>(p + 11), detail::ch(p + 19), detail::ch(p + 20),
            detail::load_alpha<4>(p + 21)};
}

template <>
inline RegShoRestriction decode<RegShoRestriction>(const std::uint8_t* p) noexcept {
    return {decode_header(p), detail::load_alpha<8>(p + 11), detail::ch(p + 19)};
}

template <>
inline MarketParticipantPosition decode<MarketParticipantPosition>(const std::uint8_t* p) noexcept {
    return {decode_header(p),   detail::load_alpha<4>(p + 11), detail::load_alpha<8>(p + 15),
            detail::ch(p + 23), detail::ch(p + 24),            detail::ch(p + 25)};
}

template <>
inline MwcbDeclineLevel decode<MwcbDeclineLevel>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11), load_be64(p + 19), load_be64(p + 27)};
}

template <>
inline MwcbStatus decode<MwcbStatus>(const std::uint8_t* p) noexcept {
    return {decode_header(p), detail::ch(p + 11)};
}

template <>
inline IpoQuotingPeriodUpdate decode<IpoQuotingPeriodUpdate>(const std::uint8_t* p) noexcept {
    return {decode_header(p), detail::load_alpha<8>(p + 11), load_be32(p + 19), detail::ch(p + 23),
            load_be32(p + 24)};
}

template <>
inline LuldAuctionCollar decode<LuldAuctionCollar>(const std::uint8_t* p) noexcept {
    return {decode_header(p),  detail::load_alpha<8>(p + 11), load_be32(p + 19), load_be32(p + 23),
            load_be32(p + 27), load_be32(p + 31)};
}

template <>
inline OperationalHalt decode<OperationalHalt>(const std::uint8_t* p) noexcept {
    return {decode_header(p), detail::load_alpha<8>(p + 11), detail::ch(p + 19), detail::ch(p + 20)};
}

template <>
inline AddOrder decode<AddOrder>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11), detail::ch(p + 19), load_be32(p + 20),
            detail::load_alpha<8>(p + 24), load_be32(p + 32)};
}

template <>
inline AddOrderMpid decode<AddOrderMpid>(const std::uint8_t* p) noexcept {
    return {decode_header(p),
            load_be64(p + 11),
            detail::ch(p + 19),
            load_be32(p + 20),
            detail::load_alpha<8>(p + 24),
            load_be32(p + 32),
            detail::load_alpha<4>(p + 36)};
}

template <>
inline OrderExecuted decode<OrderExecuted>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11), load_be32(p + 19), load_be64(p + 23)};
}

template <>
inline OrderExecutedWithPrice decode<OrderExecutedWithPrice>(const std::uint8_t* p) noexcept {
    return {decode_header(p),   load_be64(p + 11), load_be32(p + 19), load_be64(p + 23),
            detail::ch(p + 31), load_be32(p + 32)};
}

template <>
inline OrderCancel decode<OrderCancel>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11), load_be32(p + 19)};
}

template <>
inline OrderDelete decode<OrderDelete>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11)};
}

template <>
inline OrderReplace decode<OrderReplace>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11), load_be64(p + 19), load_be32(p + 27), load_be32(p + 31)};
}

template <>
inline Trade decode<Trade>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11),           detail::ch(p + 19), load_be32(p + 20),
            detail::load_alpha<8>(p + 24), load_be32(p + 32), load_be64(p + 36)};
}

template <>
inline CrossTrade decode<CrossTrade>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11), detail::load_alpha<8>(p + 19), load_be32(p + 27),
            load_be64(p + 31), detail::ch(p + 39)};
}

template <>
inline BrokenTrade decode<BrokenTrade>(const std::uint8_t* p) noexcept {
    return {decode_header(p), load_be64(p + 11)};
}

template <>
inline Noii decode<Noii>(const std::uint8_t* p) noexcept {
    Noii m;
    m.header = decode_header(p);
    m.paired_shares = load_be64(p + 11);
    m.imbalance_shares = load_be64(p + 19);
    m.imbalance_direction = detail::ch(p + 27);
    m.stock = detail::load_alpha<8>(p + 28);
    m.far_price = load_be32(p + 36);
    m.near_price = load_be32(p + 40);
    m.current_reference_price = load_be32(p + 44);
    m.cross_type = detail::ch(p + 48);
    m.price_variation_indicator = detail::ch(p + 49);
    return m;
}

template <>
inline Rpii decode<Rpii>(const std::uint8_t* p) noexcept {
    return {decode_header(p), detail::load_alpha<8>(p + 11), detail::ch(p + 19)};
}

template <>
inline DlcrPriceDiscovery decode<DlcrPriceDiscovery>(const std::uint8_t* p) noexcept {
    DlcrPriceDiscovery m;
    m.header = decode_header(p);
    m.stock = detail::load_alpha<8>(p + 11);
    m.open_eligibility_status = detail::ch(p + 19);
    m.minimum_allowable_price = load_be32(p + 20);
    m.maximum_allowable_price = load_be32(p + 24);
    m.near_execution_price = load_be32(p + 28);
    m.near_execution_time = load_be64(p + 32);
    m.lower_price_range_collar = load_be32(p + 40);
    m.upper_price_range_collar = load_be32(p + 44);
    return m;
}

// ---- Dispatch ----------------------------------------------------------------

enum class DecodeStatus { ok, unknown_type, bad_length };

// Base class for handlers: every message type is ignored by default. A handler
// derives from it, writes `using NullHandler::on;` and overrides the
// `on(const X&)` overloads it cares about. Calls are resolved at compile time
// (no virtual functions), so unused message types cost nothing.
struct NullHandler {
    template <typename M>
    void on(const M&) noexcept {}
    void on_unknown(const std::uint8_t* /*msg*/, std::size_t /*len*/) noexcept {}
    void on_bad_length(const std::uint8_t* /*msg*/, std::size_t /*len*/) noexcept {}
};

namespace detail {
template <typename M, typename H>
inline DecodeStatus deliver(const std::uint8_t* p, std::size_t len, H& h) {
    if (len != M::kSize) {
        h.on_bad_length(p, len);
        return DecodeStatus::bad_length;
    }
    h.on(decode<M>(p));
    return DecodeStatus::ok;
}
}  // namespace detail

// Decodes the message at p (len bytes, not counting the length prefix) and
// calls h.on(decoded). A message whose length does not match the spec is not
// decoded; h.on_bad_length is called instead.
template <typename H>
inline DecodeStatus dispatch(const std::uint8_t* p, std::size_t len, H& h) {
    if (len == 0) {
        h.on_bad_length(p, len);
        return DecodeStatus::bad_length;
    }
    switch (static_cast<char>(p[0])) {
        case 'S': return detail::deliver<SystemEvent>(p, len, h);
        case 'R': return detail::deliver<StockDirectory>(p, len, h);
        case 'H': return detail::deliver<StockTradingAction>(p, len, h);
        case 'Y': return detail::deliver<RegShoRestriction>(p, len, h);
        case 'L': return detail::deliver<MarketParticipantPosition>(p, len, h);
        case 'V': return detail::deliver<MwcbDeclineLevel>(p, len, h);
        case 'W': return detail::deliver<MwcbStatus>(p, len, h);
        case 'K': return detail::deliver<IpoQuotingPeriodUpdate>(p, len, h);
        case 'J': return detail::deliver<LuldAuctionCollar>(p, len, h);
        case 'h': return detail::deliver<OperationalHalt>(p, len, h);
        case 'A': return detail::deliver<AddOrder>(p, len, h);
        case 'F': return detail::deliver<AddOrderMpid>(p, len, h);
        case 'E': return detail::deliver<OrderExecuted>(p, len, h);
        case 'C': return detail::deliver<OrderExecutedWithPrice>(p, len, h);
        case 'X': return detail::deliver<OrderCancel>(p, len, h);
        case 'D': return detail::deliver<OrderDelete>(p, len, h);
        case 'U': return detail::deliver<OrderReplace>(p, len, h);
        case 'P': return detail::deliver<Trade>(p, len, h);
        case 'Q': return detail::deliver<CrossTrade>(p, len, h);
        case 'B': return detail::deliver<BrokenTrade>(p, len, h);
        case 'I': return detail::deliver<Noii>(p, len, h);
        case 'N': return detail::deliver<Rpii>(p, len, h);
        case 'O': return detail::deliver<DlcrPriceDiscovery>(p, len, h);
        default: h.on_unknown(p, len); return DecodeStatus::unknown_type;
    }
}

}  // namespace tickforge::itch
