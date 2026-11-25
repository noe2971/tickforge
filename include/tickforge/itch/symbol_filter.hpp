#pragma once
// Decides which raw ITCH messages belong to a chosen set of symbols.
//
// Rule, applied in file order:
//   * A Stock Directory ('R') message for a chosen symbol registers its locate
//     code (and is kept). 'R' messages arrive before any order for that stock,
//     so every later message can be filtered by locate alone.
//   * A message with a non-zero locate is kept iff that locate is registered.
//   * A message with locate 0 is market-wide: System Event ('S') and MWCB
//     ('V', 'W') are always kept. IPO Quoting Period Update ('K') also uses
//     locate 0 per the spec, so it is kept only if its symbol is chosen.

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "tickforge/itch/decode.hpp"
#include "tickforge/itch/endian.hpp"
#include "tickforge/itch/messages.hpp"

namespace tickforge::itch {

class SymbolFilter {
public:
    explicit SymbolFilter(const std::vector<std::string>& symbols) {
        for (const auto& s : symbols) wanted_.insert(std::string(make_symbol(s).view()));
    }

    // Returns true if the message (pointing at its type byte) should be kept.
    // Must see every message of the day in order, so it learns locates.
    bool keep(const std::uint8_t* msg, std::size_t len) {
        if (len < kHeaderSize) return false;
        const char type = static_cast<char>(msg[0]);
        const std::uint16_t locate = load_be16(msg + 1);

        if (type == StockDirectory::kType && len == StockDirectory::kSize) {
            const auto d = decode<StockDirectory>(msg);
            if (wanted_.count(std::string(d.stock.view())) != 0 && !selected_.test(locate)) {
                selected_.set(locate);
                found_.push_back({std::string(d.stock.view()), locate});
            }
        }
        if (locate != 0) return selected_.test(locate);
        if (type == IpoQuotingPeriodUpdate::kType && len == IpoQuotingPeriodUpdate::kSize) {
            return wanted_.count(std::string(decode<IpoQuotingPeriodUpdate>(msg).stock.view())) != 0;
        }
        return true;  // S, V, W: market-wide state every consumer needs
    }

    struct Found {
        std::string symbol;
        std::uint16_t locate;
    };
    // Chosen symbols whose directory message has been seen, in file order.
    const std::vector<Found>& found() const noexcept { return found_; }

private:
    std::unordered_set<std::string> wanted_;
    std::bitset<65536> selected_;
    std::vector<Found> found_;
};

}  // namespace tickforge::itch
