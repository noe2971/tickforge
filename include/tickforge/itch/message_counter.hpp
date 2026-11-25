#pragma once
// Counts ITCH messages by type and by symbol for a message-count report.

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "tickforge/itch/decode.hpp"
#include "tickforge/itch/locate_table.hpp"
#include "tickforge/itch/messages.hpp"

namespace tickforge::itch {

class MessageCounter : public NullHandler {
public:
    static constexpr std::size_t kTypes = kMessageTypes.size();
    using Counts = std::array<std::uint64_t, kTypes>;

    MessageCounter() : per_locate_(65536), closing_cross_(65536, kNoPrice) {}

    template <typename M>
    void on(const M& m) noexcept {
        count(M::kType, m.header);
    }
    void on(const StockDirectory& m) {
        locates_.add(m);
        count(StockDirectory::kType, m.header);
    }
    void on(const CrossTrade& m) noexcept {
        if (m.cross_type == 'C') closing_cross_[m.header.stock_locate] = m.cross_price;
        count(CrossTrade::kType, m.header);
    }
    void on_unknown(const std::uint8_t*, std::size_t) noexcept { ++unknown_; }
    void on_bad_length(const std::uint8_t*, std::size_t) noexcept { ++bad_length_; }

    const Counts& by_type() const noexcept { return by_type_; }
    const Counts& for_locate(std::uint16_t locate) const noexcept { return per_locate_[locate]; }
    std::uint64_t total() const noexcept { return total_; }
    std::uint64_t unknown() const noexcept { return unknown_; }
    std::uint64_t bad_length() const noexcept { return bad_length_; }
    std::uint64_t first_timestamp_ns() const noexcept { return first_ts_; }
    std::uint64_t last_timestamp_ns() const noexcept { return last_ts_; }
    const LocateTable& locates() const noexcept { return locates_; }
    // Closing-cross price for a locate (Price4), or kNoPrice if none.
    Price4 closing_cross_price(std::uint16_t locate) const noexcept { return closing_cross_[locate]; }
    static constexpr Price4 kNoPrice = 0xFFFFFFFFu;

    // One row per message type: type,name,size_bytes,count,percent
    void write_by_type_csv(std::ostream& out) const;
    // One row per symbol in the day's directory: locate, symbol, directory
    // attributes, closing cross price, total, then one column per type.
    void write_by_symbol_csv(std::ostream& out) const;

private:
    void count(char type, const Header& h) noexcept {
        static constexpr auto kIndex = [] {
            std::array<std::int8_t, 256> idx{};
            idx.fill(-1);
            for (std::size_t i = 0; i < kMessageTypes.size(); ++i)
                idx[static_cast<unsigned char>(kMessageTypes[i].type)] = static_cast<std::int8_t>(i);
            return idx;
        }();
        const auto i = static_cast<std::size_t>(kIndex[static_cast<unsigned char>(type)]);
        ++by_type_[i];
        ++per_locate_[h.stock_locate][i];
        if (total_ == 0) first_ts_ = h.timestamp_ns;
        last_ts_ = h.timestamp_ns;
        ++total_;
    }

    Counts by_type_{};
    std::vector<Counts> per_locate_;
    std::vector<Price4> closing_cross_;
    LocateTable locates_;
    std::uint64_t total_ = 0;
    std::uint64_t unknown_ = 0;
    std::uint64_t bad_length_ = 0;
    std::uint64_t first_ts_ = 0;
    std::uint64_t last_ts_ = 0;
};

// "HH:MM:SS.nnnnnnnnn" for a nanoseconds-since-midnight timestamp.
std::string format_timestamp(std::uint64_t ns_since_midnight);

}  // namespace tickforge::itch
