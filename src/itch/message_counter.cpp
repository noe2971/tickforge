#include "tickforge/itch/message_counter.hpp"

#include <cstdio>
#include <ostream>

namespace tickforge::itch {

namespace {
std::string fmt_price4(Price4 p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
    return buf;
}
std::string csv_char(char c) { return c == 0 ? std::string() : std::string(1, c); }
}  // namespace

std::string format_timestamp(std::uint64_t ns) {
    const std::uint64_t s = ns / 1'000'000'000ULL;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>((s / 60) % 60), static_cast<unsigned long long>(s % 60),
                  static_cast<unsigned long long>(ns % 1'000'000'000ULL));
    return buf;
}

void MessageCounter::write_by_type_csv(std::ostream& out) const {
    out << "type,name,size_bytes,count,percent\n";
    for (std::size_t i = 0; i < kTypes; ++i) {
        const auto& t = kMessageTypes[i];
        char pct[32];
        std::snprintf(pct, sizeof pct, "%.4f", total_ == 0 ? 0.0 : 100.0 * static_cast<double>(by_type_[i]) / static_cast<double>(total_));
        out << t.type << ',' << t.name << ',' << t.size << ',' << by_type_[i] << ',' << pct << '\n';
    }
}

void MessageCounter::write_by_symbol_csv(std::ostream& out) const {
    out << "locate,symbol,market_category,issue_classification,etp_flag,authenticity,round_lot_size,"
           "closing_cross_price,total";
    for (const auto& t : kMessageTypes) out << ',' << t.type;
    out << '\n';
    for (std::uint32_t loc = 1; loc < 65536; ++loc) {
        const auto* d = locates_.directory(static_cast<std::uint16_t>(loc));
        if (d == nullptr) continue;
        const Counts& c = per_locate_[loc];
        std::uint64_t total = 0;
        for (auto v : c) total += v;
        const Price4 cp = closing_cross_[loc];
        out << loc << ',' << d->stock.view() << ',' << csv_char(d->market_category) << ','
            << csv_char(d->issue_classification) << ',' << csv_char(d->etp_flag) << ',' << csv_char(d->authenticity)
            << ',' << d->round_lot_size << ',' << (cp == kNoPrice ? std::string() : fmt_price4(cp)) << ',' << total;
        for (auto v : c) out << ',' << v;
        out << '\n';
    }
}

}  // namespace tickforge::itch
