// itch_top_levels: export the top N price levels of one stock at any times.
//
//   itch_top_levels <file> SYMBOL TIME [TIME...] [--levels N] [--engine fast|reference] [--csv] [--compare]
//
// TIME is HH:MM:SS[.fraction], Eastern time, as in ITCH timestamps. The book
// shown is the book just before TIME: messages stamped earlier than TIME are
// applied, messages stamped at TIME or later are not (see top_levels.hpp).
// --compare runs both engines and exits 1 unless every level is identical.

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "tickforge/book/top_levels.hpp"
#include "tickforge/itch/message_counter.hpp"

using namespace tickforge;
using namespace tickforge::book;

namespace {

std::string px(Price p) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%" PRId64 ".%04" PRId64, p / 10000, p % 10000);
    return buf;
}

void print_table(const std::string& symbol, const TopLevels& t) {
    std::printf("%s, book just before %s (%llu book messages applied, last at %s)\n\n", symbol.c_str(),
                itch::format_timestamp(t.before_ns).c_str(), static_cast<unsigned long long>(t.messages_applied),
                t.messages_applied ? itch::format_timestamp(t.last_applied_ns).c_str() : "-");
    std::printf("| Level | Bid orders | Bid shares | Bid price | Ask price | Ask shares | Ask orders |\n");
    std::printf("|---:|---:|---:|---:|---:|---:|---:|\n");
    const std::size_t rows = std::max(t.bids.size(), t.asks.size());
    for (std::size_t i = 0; i < rows; ++i) {
        std::printf("| %zu |", i + 1);
        if (i < t.bids.size()) {
            std::printf(" %u | %llu | %s |", t.bids[i].orders, static_cast<unsigned long long>(t.bids[i].shares),
                        px(t.bids[i].price).c_str());
        } else {
            std::printf(" | | |");
        }
        if (i < t.asks.size()) {
            std::printf(" %s | %llu | %u |\n", px(t.asks[i].price).c_str(),
                        static_cast<unsigned long long>(t.asks[i].shares), t.asks[i].orders);
        } else {
            std::printf(" | | |\n");
        }
    }
    std::printf("\n");
}

void print_csv(const std::string& symbol, const std::vector<TopLevels>& all) {
    std::printf("symbol,before,side,level,price,shares,orders\n");
    for (const auto& t : all) {
        for (int side = 0; side < 2; ++side) {
            const auto& v = side == 0 ? t.bids : t.asks;
            for (std::size_t i = 0; i < v.size(); ++i)
                std::printf("%s,%s,%s,%zu,%s,%llu,%u\n", symbol.c_str(), itch::format_timestamp(t.before_ns).c_str(),
                            side == 0 ? "bid" : "ask", i + 1, px(v[i].price).c_str(),
                            static_cast<unsigned long long>(v[i].shares), v[i].orders);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: itch_top_levels <file> SYMBOL TIME [TIME...] [--levels N] [--engine fast|reference] "
                     "[--csv] [--compare]\n";
        return 2;
    }
    const std::string path = argv[1];
    const std::string symbol = argv[2];
    std::vector<std::uint64_t> times;
    std::size_t levels = 10;
    Engine engine = Engine::fast;
    bool csv = false, compare = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--levels" && i + 1 < argc) {
            levels = std::stoul(argv[++i]);
        } else if (a == "--engine" && i + 1 < argc) {
            engine = std::string(argv[++i]) == "reference" ? Engine::reference : Engine::fast;
        } else if (a == "--csv") {
            csv = true;
        } else if (a == "--compare") {
            compare = true;
        } else if (auto t = parse_time_of_day(a)) {
            times.push_back(*t);
        } else {
            std::cerr << "itch_top_levels: cannot read '" << a << "' (expected HH:MM:SS[.fraction])\n";
            return 2;
        }
    }
    try {
        if (compare) {
            const auto fast = top_levels_at(path, symbol, times, levels, Engine::fast);
            const auto ref = top_levels_at(path, symbol, times, levels, Engine::reference);
            std::size_t same = 0;
            for (std::size_t i = 0; i < times.size(); ++i) {
                const bool eq = fast[i] == ref[i];
                same += eq ? 1 : 0;
                std::printf("%s %s: %s (%zu bid / %zu ask levels, %llu messages applied)\n", symbol.c_str(),
                            itch::format_timestamp(times[i]).c_str(), eq ? "identical" : "DIFFERENT", fast[i].bids.size(),
                            fast[i].asks.size(), static_cast<unsigned long long>(fast[i].messages_applied));
            }
            return same == times.size() ? 0 : 1;
        }
        const auto all = top_levels_at(path, symbol, times, levels, engine);
        if (csv) {
            print_csv(symbol, all);
        } else {
            for (const auto& t : all) print_table(symbol, t);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "itch_top_levels: " << e.what() << "\n";
        return 1;
    }
}
