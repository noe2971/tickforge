// itch_engine_diff: replays a real ITCH day through FastBook and
// ReferenceBook in lockstep (PROGRESS.md, Rule D check 7).
//
//   itch_engine_diff <day.gz|day.itch|file.tfc> [output.md]
//
// After every book message: both engines must return the same status and
// values, and the affected stock's best bid and best ask (price, shares,
// order count) must match. At every whole hour and at the end, the complete
// books of every stock are compared (every level, every order in queue order).

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "tickforge/book/differential.hpp"
#include "tickforge/book/itch_replay.hpp"
#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/message_counter.hpp"
#include "tickforge/itch/stream.hpp"

using namespace tickforge;
using namespace tickforge::book;

namespace {

constexpr std::uint64_t kHour = 3'600'000'000'000ULL;

class Lockstep : public itch::NullHandler {
public:
    using itch::NullHandler::on;

    void on(const itch::AddOrder& m) {
        step(m.header, "A", [&](auto& b) { return b.add(m.order_reference_number, side_from_itch(m.side), m.price, m.shares); });
    }
    void on(const itch::AddOrderMpid& m) {
        step(m.header, "F", [&](auto& b) { return b.add(m.order_reference_number, side_from_itch(m.side), m.price, m.shares); });
    }
    void on(const itch::OrderExecuted& m) {
        step(m.header, "E", [&](auto& b) { return b.execute(m.order_reference_number, m.executed_shares); });
    }
    void on(const itch::OrderExecutedWithPrice& m) {
        step(m.header, "C", [&](auto& b) { return b.execute(m.order_reference_number, m.executed_shares); });
    }
    void on(const itch::OrderCancel& m) {
        step(m.header, "X", [&](auto& b) { return b.cancel(m.order_reference_number, m.cancelled_shares); });
    }
    void on(const itch::OrderDelete& m) {
        step(m.header, "D", [&](auto& b) { return b.remove(m.order_reference_number); });
    }
    void on(const itch::OrderReplace& m) {
        step(m.header, "U", [&](auto& b) {
            return b.replace(m.original_order_reference_number, m.new_order_reference_number, m.price, m.shares);
        });
    }

    // Compares the complete books of every stock.
    void full_compare(std::uint64_t at_ns) {
        ++full_checks_;
        fuzz::BookState a, b;
        fast_.for_each([&](std::uint16_t L, const FastBook& fb) {
            const ReferenceBook* rb = ref_.find(L);
            if (rb == nullptr) {
                mismatch(at_ns, L, "book exists only in the fast engine");
                return;
            }
            fuzz::capture(fb, a);
            fuzz::capture(*rb, b);
            orders_in_full_checks_ += b.order_count;
            if (!(a == b)) mismatch(at_ns, L, "full book differs: " + fuzz::detail::first_difference(a, b));
        });
    }

    void finish() { full_compare(last_ts_); }

    std::uint64_t book_messages() const { return book_messages_; }
    std::uint64_t mismatches() const { return mismatches_; }
    std::uint64_t full_checks() const { return full_checks_; }
    std::uint64_t orders_in_full_checks() const { return orders_in_full_checks_; }
    const std::string& log() const { return log_; }

private:
    template <typename Op>
    void step(const itch::Header& h, const char* type, Op&& op) {
        if (next_hour_ == 0) next_hour_ = (h.timestamp_ns / kHour + 1) * kHour;
        while (h.timestamp_ns >= next_hour_) {
            full_compare(next_hour_);
            next_hour_ += kHour;
        }
        last_ts_ = h.timestamp_ns;
        ++book_messages_;
        FastBook& fb = fast_.get(h.stock_locate);
        ReferenceBook& rb = ref_.get(h.stock_locate);
        const auto ra = op(fb);
        const auto rr = op(rb);
        if (!(ra == rr)) mismatch(h.timestamp_ns, h.stock_locate, std::string(type) + ": engines returned different results");
        for (Side s : {Side::buy, Side::sell}) {
            if (fb.best(s) != rb.best(s))
                mismatch(h.timestamp_ns, h.stock_locate, std::string(type) + ": best " + (s == Side::buy ? "bid" : "ask") + " differs");
        }
    }

    void mismatch(std::uint64_t ts, std::uint16_t locate, const std::string& what) {
        if (++mismatches_ <= 20)
            log_ += "- " + itch::format_timestamp(ts) + " locate " + std::to_string(locate) + ": " + what + "\n";
    }

    BookSet<FastBook> fast_;
    BookSet<ReferenceBook> ref_;
    std::uint64_t next_hour_ = 0;
    std::uint64_t last_ts_ = 0;
    std::uint64_t book_messages_ = 0;
    std::uint64_t mismatches_ = 0;
    std::uint64_t full_checks_ = 0;
    std::uint64_t orders_in_full_checks_ = 0;
    std::string log_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: itch_engine_diff <input> [output.md]\n";
        return 2;
    }
    const std::string input = argv[1];
    try {
        std::unique_ptr<io::ByteSource> src;
        if (input.size() > 4 && input.compare(input.size() - 4, 4, ".tfc") == 0) {
            src = std::make_unique<itch::CompactReader>(input);
        } else {
            src = io::open_byte_source(input);
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto ls = std::make_unique<Lockstep>();
        const auto st = itch::parse_stream(*src, *ls);
        ls->finish();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        std::ostringstream md;
        md << "# Real-day differential: FastBook vs ReferenceBook\n\n";
        md << "Generated by `tools/itch_engine_diff` (method: `PROGRESS.md`, Rule D check 7) on `" << input << "`.\n\n";
        md << "| | |\n|---|---|\n";
        md << "| Messages read | " << st.messages << " |\n";
        md << "| Book messages applied to both engines | " << ls->book_messages() << " |\n";
        md << "| Per-message checks (result + best bid + best ask) | " << ls->book_messages() << " |\n";
        md << "| Full-book comparisons (all stocks; whole hours + end of day) | " << ls->full_checks() << " |\n";
        md << "| Orders compared in those full-book checks | " << ls->orders_in_full_checks() << " |\n";
        md << "| Mismatches | **" << ls->mismatches() << "** |\n";
        md << "| Wall time | " << static_cast<long long>(secs) << " s |\n\n";
        if (ls->mismatches() != 0) md << "First mismatches:\n\n" << ls->log() << "\n";
        std::cout << md.str();
        if (argc > 2) std::ofstream(argv[2]) << md.str();
        return ls->mismatches() == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "itch_engine_diff: " << e.what() << "\n";
        return 1;
    }
}
