#pragma once
// ReplayValidator: replays a full ITCH day into FastBooks and checks that the
// feed and the rebuilt books are consistent (PROGRESS.md, Rule D).
//
// Checks, all counted, every failure kept as an "exception" with examples:
//   1. every execution (E, C) hits a live order of that stock with enough
//      shares; an E (which has no price field) hits an order at the best
//      price on its side; C prices are compared with the order's price;
//   2. X / D / U hit live orders, X never cancels more than is left, adds
//      and replace targets are new references;
//   3. the book is never crossed (or locked) during continuous trading;
//   4. a ledger kept *independently of the engine* reconciles every order's
//      shares to zero or to the end-of-day book;
//   5. hidden-order trades (P), crosses (Q) and broken trades (B) are counted;
//   6. timestamps never go backwards.
//
// The ledger is a plain std::unordered_map with its own arithmetic, so a bug
// in the FastBook's share accounting shows up as a disagreement instead of
// being repeated by the check.

#include <array>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "tickforge/book/fast_book.hpp"
#include "tickforge/book/itch_replay.hpp"
#include "tickforge/itch/locate_table.hpp"
#include "tickforge/itch/messages.hpp"

namespace tickforge::book {

// Every kind of exception the validator can record.
enum class Issue : std::uint8_t {
    exec_unknown_order,       // E/C for a reference that is not live in that stock's book
    exec_too_many_shares,     // E/C for more shares than the order has left
    exec_not_at_best,         // E against an order that is not at the best price on its side
    cancel_unknown_order,     // X for a reference that is not live
    cancel_too_many_shares,   // X for more shares than are left
    delete_unknown_order,     // D for a reference that is not live
    replace_unknown_order,    // U whose original reference is not live
    replace_duplicate_new,    // U whose new reference is already live
    add_duplicate_order,      // A/F with a reference that is already live
    bad_values,               // 0 shares, price <= 0, or a side that is not B/S
    ref_in_other_stock,       // E/C/X/D/U whose reference is live, but in a different stock
    crossed_continuous,       // best bid > best ask during continuous trading
    locked_continuous,        // best bid = best ask during continuous trading
    ledger_mismatch,          // the independent ledger and the engine disagree
    reconciliation_failure,   // a stock's shares do not add up at the end of the day
    timestamp_backwards,      // a message is stamped earlier than the one before it
};
inline constexpr std::size_t kIssueCount = 16;
const char* to_string(Issue) noexcept;
const char* describe(Issue) noexcept;

struct IssueExample {
    std::uint64_t ts = 0;
    std::uint16_t locate = 0;
    std::uint64_t ref = 0;
    std::string detail;
};

struct IssueLog {
    std::uint64_t count = 0;
    std::vector<IssueExample> examples;   // the first few, in file order
    std::map<std::uint16_t, std::uint64_t> by_locate;
};

// One stretch during which a stock's book stayed crossed (or locked).
struct CrossEpisode {
    std::uint16_t locate = 0;
    bool crossed = false;  // false = locked
    bool continuous = false;
    char trading_state = 0;
    std::uint64_t start_ts = 0;
    std::uint64_t end_ts = 0;     // timestamp of the message that ended it
    std::uint64_t messages = 0;   // book messages after which it was still crossed/locked
    Price bid = 0;
    Price ask = 0;
    bool open_at_end = false;
};

struct CrossStats {
    std::uint64_t episodes = 0;
    std::uint64_t messages = 0;
    std::uint64_t same_timestamp = 0;           // resolved by a message with the same timestamp
    std::array<std::uint64_t, 5> duration{};    // 0 ns, <1 us, <1 ms, <1 s, >= 1 s
    std::vector<CrossEpisode> examples;
    std::map<std::uint16_t, std::uint64_t> by_locate;
};

struct SymbolStats {
    std::uint64_t book_messages = 0;
    std::uint64_t adds = 0;
    std::uint64_t executions = 0;
    std::uint64_t added_shares = 0;     // A/F shares plus the new size of every U
    std::uint64_t executed_shares = 0;
    std::uint64_t cancelled_shares = 0;
    std::uint64_t deleted_shares = 0;   // shares still open when a D removed the order
    std::uint64_t replaced_shares = 0;  // shares still open when a U removed the order
    std::uint64_t eod_ledger_shares = 0;
    std::uint64_t eod_ledger_orders = 0;
    std::uint64_t eod_book_shares = 0;
    std::uint64_t eod_book_orders = 0;
    std::size_t peak_orders = 0;
    std::size_t peak_bid_levels = 0;
    std::size_t peak_ask_levels = 0;
    Price min_price = 0;
    Price max_price = 0;
};

// How an execution-with-price (C) compared with the order's resting price.
struct PricedExecStats {
    // [printable Y=0 / N=1][equal=0 / better for the resting order=1 / worse=2]
    std::uint64_t count[2][3] = {};
    std::uint64_t shares[2][3] = {};
    std::uint64_t not_at_best[2] = {};
    std::uint64_t during_market_hours[2] = {};
};

class ReplayValidator {
public:
    ReplayValidator();

    // ITCH handler interface (see itch/decode.hpp).
    void on(const itch::SystemEvent& m);
    void on(const itch::StockDirectory& m);
    void on(const itch::StockTradingAction& m);
    void on(const itch::AddOrder& m);
    void on(const itch::AddOrderMpid& m);
    void on(const itch::OrderExecuted& m);
    void on(const itch::OrderExecutedWithPrice& m);
    void on(const itch::OrderCancel& m);
    void on(const itch::OrderDelete& m);
    void on(const itch::OrderReplace& m);
    void on(const itch::Trade& m);
    void on(const itch::CrossTrade& m);
    void on(const itch::BrokenTrade& m);
    template <typename M>
    void on(const M& m) {
        tick(m.header);
    }
    void on_unknown(const std::uint8_t*, std::size_t) noexcept { ++unknown_messages_; }
    void on_bad_length(const std::uint8_t*, std::size_t) noexcept { ++bad_length_messages_; }

    // End-of-day checks (reconciliation). Call once, after the last message.
    void finish();

    // Results.
    const IssueLog& issue(Issue i) const noexcept { return issues_[static_cast<std::size_t>(i)]; }
    std::uint64_t issue_total() const noexcept;
    const CrossStats& cross_stats(bool crossed, bool continuous) const noexcept {
        return cross_[crossed ? 1 : 0][continuous ? 1 : 0];
    }
    const SymbolStats* symbol(std::uint16_t locate) const noexcept;
    const itch::LocateTable& locates() const noexcept { return locates_; }
    const BookSet<FastBook>& books() const noexcept { return books_; }

    // Report writers.
    void write_markdown(std::ostream& out, const std::string& input_name, const std::vector<std::string>& focus) const;
    void write_peaks_csv(std::ostream& out) const;

private:
    struct LedgerEntry {
        std::uint16_t locate;
        std::uint32_t open;  // shares not yet executed or cancelled
        std::uint8_t origin; // how the order was created (see Origin), for the post-hoc breakdowns
    };
    // 0: A/F add; 1: U at the same price with fewer shares; 2: U at the same
    // price with the same or more shares; 3: U at a new price.
    static constexpr int kOrigins = 4;

    void tick(const itch::Header& h);
    SymbolStats& stats(std::uint16_t locate);
    // Counts the issue; returns the new example to fill in, or nullptr once
    // enough examples of that kind are stored.
    IssueExample* record(Issue i, const itch::Header& h, std::uint64_t ref);
    void add(const itch::Header& h, std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price);
    void execution(const itch::Header& h, std::uint64_t ref, std::uint32_t shares, bool priced, std::uint32_t price,
                   char printable);
    bool other_stock(const itch::Header& h, std::uint64_t ref, const char* what);
    void after_book_message(const itch::Header& h);
    void explain_not_front(const itch::Header& h, std::uint64_t ref, const OrderView& o, const FastBook& b, int origin);
    void check_cross(const itch::Header& h);
    void close_episode(std::uint16_t locate, std::uint64_t end_ts, bool open_at_end);
    bool continuous(std::uint16_t locate) const noexcept;

    BookSet<FastBook> books_;
    itch::LocateTable locates_;
    std::unordered_map<std::uint64_t, LedgerEntry> ledger_;
    std::vector<SymbolStats> symbols_;           // by locate
    std::vector<char> has_symbol_stats_;         // by locate
    std::vector<char> trading_state_;            // by locate; 0 = no 'H' message yet
    std::vector<CrossEpisode> open_episode_;     // by locate
    std::vector<char> episode_active_;           // by locate
    std::array<IssueLog, kIssueCount> issues_{};
    CrossStats cross_[2][2];                     // [crossed][continuous]

    std::uint64_t last_ts_ = 0;
    std::uint64_t messages_ = 0;
    std::uint64_t unknown_messages_ = 0;
    std::uint64_t bad_length_messages_ = 0;
    bool market_open_ = false;
    std::map<char, std::uint64_t> system_events_;  // event code -> timestamp
    std::array<std::uint64_t, 128> book_messages_by_type_{};

    // Executions.
    std::uint64_t exec_count_[2] = {};       // [E=0 / C=1]
    std::uint64_t exec_at_front_[2] = {};    // executed order was first in its queue
    std::uint64_t exec_shares_[2] = {};
    PricedExecStats priced_;
    std::uint64_t cancel_all_remaining_ = 0;  // X that cancelled every remaining share

    // Messages that do not touch the visible book.
    std::uint64_t trades_ = 0, trade_shares_ = 0, trades_nonzero_ref_ = 0;
    std::map<char, std::pair<std::uint64_t, std::uint64_t>> crosses_;  // cross type -> (count, shares)
    std::uint64_t broken_trades_ = 0;
    std::map<char, std::uint64_t> trading_actions_;  // trading state -> count
    std::vector<IssueExample> halts_;               // H messages with a state other than T

    // Post-hoc descriptive breakdowns, added after the first full-day run to
    // explain what it found. They are not pass/fail rules.
    std::uint64_t c_gap_[2][6] = {};  // [printable Y/N][|C price - resting price|: 1-49, 50, 51-99, 100, 101-999, >=1000]
    std::uint64_t e_by_origin_[2][kOrigins] = {};  // [executed order was at front? no/yes][its origin]
    std::uint64_t e_not_front_market_hours_ = 0;
    std::uint64_t e_not_front_position_[4] = {};   // queue position 2, 3, 4-10, >10
    std::uint64_t ahead_by_origin_[kOrigins] = {}; // orders ahead of a not-at-front E, by origin
    std::vector<IssueExample> e_not_front_examples_;
    struct NonPrintable {
        std::uint64_t ts;
        std::uint16_t locate;
        std::uint8_t rel;  // 0 equal, 1 better for the resting order, 2 worse
    };
    std::vector<NonPrintable> nonprintable_execs_;
    std::uint64_t np_cross_by_rel_[2][3] = {};     // [near a cross within 1 s? no/yes][price relation]
    std::uint64_t e_not_front_by_hour_[24] = {};   // hour of day (ET)
    std::uint64_t e_not_front_first_second_ = 0;   // within 1 s after the market-open event
    struct CrossMark {
        std::uint16_t locate;
        std::uint64_t ts;
        char type;
    };
    std::vector<CrossMark> cross_marks_;
    std::uint64_t np_near_cross_[5] = {};  // nearest cross of that stock within 1 s: O, C, H, other, none

    // End of day.
    bool finished_ = false;
    std::uint64_t eod_orders_ = 0, eod_shares_ = 0, eod_books_nonempty_ = 0;
    std::uint64_t books_without_trading_action_ = 0;
    std::uint64_t symbols_reconciled_ = 0, symbols_with_activity_ = 0;
    std::size_t ledger_peak_ = 0;
};

}  // namespace tickforge::book
