#pragma once
// Maps ITCH stock locate codes to symbols.
//
// Each trading day Nasdaq assigns every security a small integer "stock
// locate" (1..65535) and announces the mapping in Stock Directory ('R')
// messages before the market opens. Every later message carries only the
// 2-byte locate, not the 8-byte symbol, so a consumer builds this table from
// the 'R' messages and uses it to name everything else. Locate 0 means "not
// about one stock" (system events, market-wide circuit breakers).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tickforge/itch/messages.hpp"

namespace tickforge::itch {

class LocateTable {
public:
    LocateTable() : entries_(65536) {}

    // Records the locate -> symbol mapping (and the full directory record).
    void add(const StockDirectory& m) {
        Entry& e = entries_[m.header.stock_locate];
        if (!e.known) ++count_;
        e.known = true;
        e.directory = m;
        by_symbol_[std::string(m.stock.view())] = m.header.stock_locate;
    }

    bool known(std::uint16_t locate) const noexcept { return entries_[locate].known; }

    // Symbol for a locate, or "" if no directory message has been seen.
    std::string_view symbol(std::uint16_t locate) const noexcept {
        const Entry& e = entries_[locate];
        return e.known ? e.directory.stock.view() : std::string_view{};
    }

    const StockDirectory* directory(std::uint16_t locate) const noexcept {
        const Entry& e = entries_[locate];
        return e.known ? &e.directory : nullptr;
    }

    std::optional<std::uint16_t> find(std::string_view symbol) const {
        auto it = by_symbol_.find(std::string(symbol));
        if (it == by_symbol_.end()) return std::nullopt;
        return it->second;
    }

    std::size_t size() const noexcept { return count_; }

private:
    struct Entry {
        bool known = false;
        StockDirectory directory;
    };
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::uint16_t> by_symbol_;
    std::size_t count_ = 0;
};

}  // namespace tickforge::itch
