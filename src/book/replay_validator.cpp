#include "tickforge/book/replay_validator.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <ostream>
#include <string>
#include <utility>

#include "tickforge/itch/message_counter.hpp"

namespace tickforge::book {

namespace {

constexpr std::size_t kMaxExamples = 12;
constexpr std::size_t kMaxCrossExamples = 15;
constexpr std::size_t kMaxHalts = 40;

std::string px(Price p) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%" PRId64 ".%04" PRId64, p / 10000, p % 10000);
    return buf;
}

std::string ts(std::uint64_t ns) { return itch::format_timestamp(ns); }

std::string num(std::uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

std::string pct(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f%%", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
    return buf;
}

const char* side_name(Side s) { return s == Side::buy ? "buy" : "sell"; }

}  // namespace

const char* to_string(Issue i) noexcept {
    switch (i) {
        case Issue::exec_unknown_order: return "exec_unknown_order";
        case Issue::exec_too_many_shares: return "exec_too_many_shares";
        case Issue::exec_not_at_best: return "exec_not_at_best";
        case Issue::cancel_unknown_order: return "cancel_unknown_order";
        case Issue::cancel_too_many_shares: return "cancel_too_many_shares";
        case Issue::delete_unknown_order: return "delete_unknown_order";
        case Issue::replace_unknown_order: return "replace_unknown_order";
        case Issue::replace_duplicate_new: return "replace_duplicate_new";
        case Issue::add_duplicate_order: return "add_duplicate_order";
        case Issue::bad_values: return "bad_values";
        case Issue::ref_in_other_stock: return "ref_in_other_stock";
        case Issue::crossed_continuous: return "crossed_continuous";
        case Issue::locked_continuous: return "locked_continuous";
        case Issue::ledger_mismatch: return "ledger_mismatch";
        case Issue::reconciliation_failure: return "reconciliation_failure";
        case Issue::timestamp_backwards: return "timestamp_backwards";
    }
    return "?";
}

const char* describe(Issue i) noexcept {
    switch (i) {
        case Issue::exec_unknown_order: return "E/C refers to an order that is not live in that stock's book";
        case Issue::exec_too_many_shares: return "E/C executes more shares than the order has left";
        case Issue::exec_not_at_best: return "E executes an order that is not at the best price on its side";
        case Issue::cancel_unknown_order: return "X refers to an order that is not live";
        case Issue::cancel_too_many_shares: return "X cancels more shares than the order has left";
        case Issue::delete_unknown_order: return "D refers to an order that is not live";
        case Issue::replace_unknown_order: return "U's original order is not live";
        case Issue::replace_duplicate_new: return "U's new reference is already live";
        case Issue::add_duplicate_order: return "A/F reuses a reference that is still live";
        case Issue::bad_values: return "0 shares, price <= 0, or a side that is not B/S";
        case Issue::ref_in_other_stock: return "reference is live, but in a different stock's book";
        case Issue::crossed_continuous: return "best bid > best ask during continuous trading (episodes)";
        case Issue::locked_continuous: return "best bid = best ask during continuous trading (episodes)";
        case Issue::ledger_mismatch: return "independent share ledger and engine disagree";
        case Issue::reconciliation_failure: return "a stock's shares do not add up at end of day";
        case Issue::timestamp_backwards: return "message timestamp earlier than the previous message";
    }
    return "?";
}

ReplayValidator::ReplayValidator()
    : symbols_(65536),
      has_symbol_stats_(65536, 0),
      trading_state_(65536, 0),
      open_episode_(65536),
      episode_active_(65536, 0) {}

std::uint64_t ReplayValidator::issue_total() const noexcept {
    std::uint64_t n = 0;
    for (const auto& l : issues_) n += l.count;
    return n;
}

const SymbolStats* ReplayValidator::symbol(std::uint16_t locate) const noexcept {
    return has_symbol_stats_[locate] ? &symbols_[locate] : nullptr;
}

SymbolStats& ReplayValidator::stats(std::uint16_t locate) {
    has_symbol_stats_[locate] = 1;
    return symbols_[locate];
}

IssueExample* ReplayValidator::record(Issue i, const itch::Header& h, std::uint64_t ref) {
    IssueLog& log = issues_[static_cast<std::size_t>(i)];
    ++log.count;
    ++log.by_locate[h.stock_locate];
    if (log.examples.size() >= kMaxExamples) return nullptr;
    log.examples.push_back({h.timestamp_ns, h.stock_locate, ref, {}});
    return &log.examples.back();
}

void ReplayValidator::tick(const itch::Header& h) {
    ++messages_;
    if (h.timestamp_ns < last_ts_) {
        if (auto* e = record(Issue::timestamp_backwards, h, 0)) e->detail = "previous message at " + ts(last_ts_);
    }
    last_ts_ = h.timestamp_ns;
}

bool ReplayValidator::continuous(std::uint16_t locate) const noexcept {
    const char st = trading_state_[locate];
    return market_open_ && (st == 'T' || st == 0);
}

// ---- Market state --------------------------------------------------------------

void ReplayValidator::on(const itch::SystemEvent& m) {
    tick(m.header);
    system_events_[m.event_code] = m.header.timestamp_ns;
    if (m.event_code == 'Q') market_open_ = true;
    if (m.event_code == 'M') market_open_ = false;
}

void ReplayValidator::on(const itch::StockDirectory& m) {
    tick(m.header);
    locates_.add(m);
}

void ReplayValidator::on(const itch::StockTradingAction& m) {
    tick(m.header);
    trading_state_[m.header.stock_locate] = m.trading_state;
    ++trading_actions_[m.trading_state];
    if (m.trading_state != 'T' && halts_.size() < kMaxHalts) {
        halts_.push_back({m.header.timestamp_ns, m.header.stock_locate, 0,
                          std::string("state ") + m.trading_state + ", reason '" + std::string(m.reason.view()) + "'"});
    }
}

// ---- Book messages ---------------------------------------------------------------

void ReplayValidator::on(const itch::AddOrder& m) {
    tick(m.header);
    ++book_messages_by_type_['A'];
    add(m.header, m.order_reference_number, m.side, m.shares, m.price);
}

void ReplayValidator::on(const itch::AddOrderMpid& m) {
    tick(m.header);
    ++book_messages_by_type_['F'];
    add(m.header, m.order_reference_number, m.side, m.shares, m.price);
}

void ReplayValidator::on(const itch::OrderExecuted& m) {
    tick(m.header);
    ++book_messages_by_type_['E'];
    execution(m.header, m.order_reference_number, m.executed_shares, false, 0, 'Y');
}

void ReplayValidator::on(const itch::OrderExecutedWithPrice& m) {
    tick(m.header);
    ++book_messages_by_type_['C'];
    execution(m.header, m.order_reference_number, m.executed_shares, true, m.execution_price, m.printable);
}

bool ReplayValidator::other_stock(const itch::Header& h, std::uint64_t ref, const char* what) {
    const auto it = ledger_.find(ref);
    if (it == ledger_.end() || it->second.locate == h.stock_locate) return false;
    if (auto* e = record(Issue::ref_in_other_stock, h, ref))
        e->detail = std::string(what) + " says locate " + std::to_string(h.stock_locate) + ", order lives in locate " +
                    std::to_string(it->second.locate);
    return true;
}

void ReplayValidator::add(const itch::Header& h, std::uint64_t ref, char side_c, std::uint32_t shares,
                          std::uint32_t price) {
    const std::uint16_t L = h.stock_locate;
    SymbolStats& st = stats(L);
    if (side_c != 'B' && side_c != 'S') {
        if (auto* e = record(Issue::bad_values, h, ref)) e->detail = std::string("side byte '") + side_c + "'";
        return;
    }
    const auto it = ledger_.find(ref);
    if (it != ledger_.end() && it->second.locate != L) {
        if (auto* e = record(Issue::ref_in_other_stock, h, ref))
            e->detail = "add reuses a reference live in locate " + std::to_string(it->second.locate);
        return;
    }
    FastBook& b = books_.get(L);
    const Status s = b.add(ref, side_from_itch(side_c), price, shares);
    if (s == Status::duplicate_order) {
        if (auto* e = record(Issue::add_duplicate_order, h, ref)) e->detail = "reference already live";
        return;
    }
    if (s != Status::ok) {
        if (auto* e = record(Issue::bad_values, h, ref))
            e->detail = std::string("add rejected: ") + to_string(s) + " (" + std::to_string(shares) + " sh @ " +
                        px(price) + ")";
        return;
    }
    if (it != ledger_.end()) {
        if (auto* e = record(Issue::ledger_mismatch, h, ref)) e->detail = "engine accepted an add the ledger holds open";
    }
    ledger_[ref] = LedgerEntry{L, shares, 0};
    ledger_peak_ = std::max(ledger_peak_, ledger_.size());
    ++st.adds;
    st.added_shares += shares;
    if (st.min_price == 0 || price < st.min_price) st.min_price = price;
    if (price > st.max_price) st.max_price = price;
    after_book_message(h);
}

void ReplayValidator::execution(const itch::Header& h, std::uint64_t ref, std::uint32_t shares, bool priced,
                                std::uint32_t price, char printable) {
    const int k = priced ? 1 : 0;
    ++exec_count_[k];
    exec_shares_[k] += shares;
    const std::uint16_t L = h.stock_locate;
    SymbolStats& st = stats(L);
    ++st.executions;
    FastBook& b = books_.get(L);
    const auto o = b.find(ref);
    if (!o) {
        if (!other_stock(h, ref, priced ? "C" : "E")) {
            if (auto* e = record(Issue::exec_unknown_order, h, ref))
                e->detail = std::string(priced ? "C" : "E") + " for " + std::to_string(shares) + " sh";
        }
        return;
    }
    if (shares == 0) {
        if (auto* e = record(Issue::bad_values, h, ref)) e->detail = "execution of 0 shares";
        return;
    }
    if (shares > o->qty) {
        if (auto* e = record(Issue::exec_too_many_shares, h, ref))
            e->detail = std::string(priced ? "C" : "E") + " executes " + std::to_string(shares) + " sh, " +
                        std::to_string(o->qty) + " left";
        return;
    }
    const auto best = b.best(o->side);
    const bool at_best = best && best->price == o->price;
    if (o->at_front) ++exec_at_front_[k];
    if (!priced) {
        const auto lit = ledger_.find(ref);
        const int origin = lit != ledger_.end() ? lit->second.origin : 0;
        ++e_by_origin_[o->at_front ? 1 : 0][origin];
        if (!o->at_front) explain_not_front(h, ref, *o, b, origin);
    }
    if (priced) {
        const std::uint64_t gap = static_cast<std::uint64_t>(price > o->price ? price - o->price : o->price - price);
        if (gap != 0) {
            const int bucket = gap < 50 ? 0 : gap == 50 ? 1 : gap < 100 ? 2 : gap == 100 ? 3 : gap < 1000 ? 4 : 5;
            ++c_gap_[printable == 'N' ? 1 : 0][bucket];
        }
        if (printable == 'N') {
            const Price ep = price;
            const std::uint8_t rel = ep == o->price ? 0 : ((o->side == Side::buy ? ep < o->price : ep > o->price) ? 1 : 2);
            nonprintable_execs_.push_back({h.timestamp_ns, L, rel});
        }
    }
    if (!priced) {
        if (!at_best) {
            if (auto* e = record(Issue::exec_not_at_best, h, ref))
                e->detail = std::string(side_name(o->side)) + " order @ " + px(o->price) + ", best " +
                            (best ? px(best->price) : std::string("none"));
        }
    } else {
        const int pr = printable == 'N' ? 1 : 0;
        const Price ep = price;
        const int rel = ep == o->price ? 0 : ((o->side == Side::buy ? ep < o->price : ep > o->price) ? 1 : 2);
        ++priced_.count[pr][rel];
        priced_.shares[pr][rel] += shares;
        if (!at_best) ++priced_.not_at_best[pr];
        if (market_open_) ++priced_.during_market_hours[pr];
    }

    const ExecResult r = b.execute(ref, shares);
    const auto it = ledger_.find(ref);
    if (r.status != Status::ok || it == ledger_.end() || it->second.open < shares) {
        if (auto* e = record(Issue::ledger_mismatch, h, ref)) e->detail = "execution: engine/ledger disagree";
    } else {
        it->second.open -= shares;
        st.executed_shares += shares;
        if (it->second.open != r.remaining) {
            if (auto* e = record(Issue::ledger_mismatch, h, ref))
                e->detail = "after execution ledger has " + std::to_string(it->second.open) + ", engine " +
                            std::to_string(r.remaining);
        }
        if (it->second.open == 0) ledger_.erase(it);
    }
    after_book_message(h);
}

void ReplayValidator::on(const itch::OrderCancel& m) {
    tick(m.header);
    ++book_messages_by_type_['X'];
    const auto& h = m.header;
    const std::uint64_t ref = m.order_reference_number;
    SymbolStats& st = stats(h.stock_locate);
    FastBook& b = books_.get(h.stock_locate);
    const auto o = b.find(ref);
    if (!o) {
        if (!other_stock(h, ref, "X")) {
            if (auto* e = record(Issue::cancel_unknown_order, h, ref))
                e->detail = "X for " + std::to_string(m.cancelled_shares) + " sh";
        }
        return;
    }
    if (m.cancelled_shares == 0) {
        if (auto* e = record(Issue::bad_values, h, ref)) e->detail = "cancel of 0 shares";
        return;
    }
    if (m.cancelled_shares > o->qty) {
        if (auto* e = record(Issue::cancel_too_many_shares, h, ref))
            e->detail = "X cancels " + std::to_string(m.cancelled_shares) + " sh, " + std::to_string(o->qty) + " left";
        return;
    }
    if (m.cancelled_shares == o->qty) ++cancel_all_remaining_;
    const Status s = b.cancel(ref, m.cancelled_shares);
    const auto it = ledger_.find(ref);
    if (s != Status::ok || it == ledger_.end() || it->second.open < m.cancelled_shares) {
        if (auto* e = record(Issue::ledger_mismatch, h, ref)) e->detail = "cancel: engine/ledger disagree";
    } else {
        it->second.open -= m.cancelled_shares;
        st.cancelled_shares += m.cancelled_shares;
        const bool engine_has = b.find(ref).has_value();
        if (engine_has != (it->second.open > 0)) {
            if (auto* e = record(Issue::ledger_mismatch, h, ref)) e->detail = "cancel: order presence disagrees";
        }
        if (it->second.open == 0) ledger_.erase(it);
    }
    after_book_message(h);
}

void ReplayValidator::on(const itch::OrderDelete& m) {
    tick(m.header);
    ++book_messages_by_type_['D'];
    const auto& h = m.header;
    const std::uint64_t ref = m.order_reference_number;
    SymbolStats& st = stats(h.stock_locate);
    FastBook& b = books_.get(h.stock_locate);
    const auto o = b.find(ref);
    if (!o) {
        if (!other_stock(h, ref, "D")) record(Issue::delete_unknown_order, h, ref);
        return;
    }
    const auto it = ledger_.find(ref);
    if (it == ledger_.end() || it->second.open != o->qty) {
        if (auto* e = record(Issue::ledger_mismatch, h, ref)) e->detail = "delete: open shares disagree";
    }
    st.deleted_shares += it != ledger_.end() ? it->second.open : o->qty;
    b.remove(ref);
    if (it != ledger_.end()) ledger_.erase(it);
    after_book_message(h);
}

void ReplayValidator::on(const itch::OrderReplace& m) {
    tick(m.header);
    ++book_messages_by_type_['U'];
    const auto& h = m.header;
    const std::uint64_t old_ref = m.original_order_reference_number;
    const std::uint64_t new_ref = m.new_order_reference_number;
    SymbolStats& st = stats(h.stock_locate);
    FastBook& b = books_.get(h.stock_locate);
    const auto o = b.find(old_ref);
    if (!o) {
        if (!other_stock(h, old_ref, "U")) {
            if (auto* e = record(Issue::replace_unknown_order, h, old_ref)) e->detail = "new ref " + std::to_string(new_ref);
        }
        return;
    }
    if (new_ref == old_ref || b.find(new_ref) || ledger_.count(new_ref) != 0) {
        if (auto* e = record(Issue::replace_duplicate_new, h, old_ref)) e->detail = "new ref " + std::to_string(new_ref);
        return;
    }
    const Status s = b.replace(old_ref, new_ref, m.price, m.shares);
    if (s != Status::ok) {
        if (auto* e = record(Issue::bad_values, h, old_ref))
            e->detail = std::string("replace rejected: ") + to_string(s) + " (" + std::to_string(m.shares) + " sh @ " +
                        px(m.price) + ")";
        return;
    }
    const auto it = ledger_.find(old_ref);
    if (it == ledger_.end() || it->second.open != o->qty) {
        if (auto* e = record(Issue::ledger_mismatch, h, old_ref)) e->detail = "replace: open shares disagree";
    }
    st.replaced_shares += it != ledger_.end() ? it->second.open : o->qty;
    if (it != ledger_.end()) ledger_.erase(it);
    const std::uint8_t origin = static_cast<Price>(m.price) != o->price ? 3 : (m.shares < o->qty ? 1 : 2);
    ledger_[new_ref] = LedgerEntry{h.stock_locate, m.shares, origin};
    ledger_peak_ = std::max(ledger_peak_, ledger_.size());
    st.added_shares += m.shares;
    if (st.min_price == 0 || m.price < st.min_price) st.min_price = m.price;
    if (m.price > st.max_price) st.max_price = m.price;
    after_book_message(h);
}

void ReplayValidator::after_book_message(const itch::Header& h) {
    SymbolStats& st = symbols_[h.stock_locate];
    const FastBook& b = *books_.find(h.stock_locate);
    ++st.book_messages;
    st.peak_orders = std::max(st.peak_orders, b.order_count());
    st.peak_bid_levels = std::max(st.peak_bid_levels, b.level_count(Side::buy));
    st.peak_ask_levels = std::max(st.peak_ask_levels, b.level_count(Side::sell));
    check_cross(h);
}

// ---- Crossed / locked books -------------------------------------------------------

void ReplayValidator::check_cross(const itch::Header& h) {
    const std::uint16_t L = h.stock_locate;
    const FastBook& b = *books_.find(L);
    const auto bid = b.best(Side::buy);
    const auto ask = b.best(Side::sell);
    int state = 0;  // 0 normal, 1 locked, 2 crossed
    if (bid && ask) state = bid->price > ask->price ? 2 : (bid->price == ask->price ? 1 : 0);

    if (episode_active_[L]) {
        CrossEpisode& ep = open_episode_[L];
        if (state == (ep.crossed ? 2 : 1)) {
            ++ep.messages;
            return;
        }
        close_episode(L, h.timestamp_ns, false);
    }
    if (state != 0) {
        CrossEpisode& ep = open_episode_[L];
        ep = CrossEpisode{L, state == 2, continuous(L), trading_state_[L], h.timestamp_ns, 0, 1,
                          bid->price, ask->price, false};
        episode_active_[L] = 1;
    }
}

void ReplayValidator::close_episode(std::uint16_t locate, std::uint64_t end_ts, bool open_at_end) {
    CrossEpisode& ep = open_episode_[locate];
    ep.end_ts = end_ts;
    ep.open_at_end = open_at_end;
    CrossStats& cs = cross_[ep.crossed ? 1 : 0][ep.continuous ? 1 : 0];
    ++cs.episodes;
    cs.messages += ep.messages;
    const std::uint64_t d = end_ts - ep.start_ts;
    if (!open_at_end && d == 0) ++cs.same_timestamp;
    const std::size_t bucket = d == 0 ? 0 : d < 1'000 ? 1 : d < 1'000'000 ? 2 : d < 1'000'000'000 ? 3 : 4;
    ++cs.duration[bucket];
    ++cs.by_locate[locate];
    if (cs.examples.size() < kMaxCrossExamples) cs.examples.push_back(ep);
    if (ep.continuous) {
        const itch::Header h{locate, 0, ep.start_ts};
        if (auto* e = record(ep.crossed ? Issue::crossed_continuous : Issue::locked_continuous, h, 0))
            e->detail = "bid " + px(ep.bid) + " / ask " + px(ep.ask) + ", lasted " + std::to_string(d) + " ns over " +
                        std::to_string(ep.messages) + " messages";
    }
    episode_active_[locate] = 0;
}

// ---- Messages that do not touch the visible book ---------------------------------

void ReplayValidator::on(const itch::Trade& m) {
    tick(m.header);
    ++trades_;
    trade_shares_ += m.shares;
    if (m.order_reference_number != 0) ++trades_nonzero_ref_;
}

void ReplayValidator::explain_not_front(const itch::Header& h, std::uint64_t ref, const OrderView& o,
                                        const FastBook& b, int origin) {
    if (market_open_) ++e_not_front_market_hours_;
    ++e_not_front_by_hour_[(h.timestamp_ns / 3'600'000'000'000ULL) % 24];
    const auto open_it = system_events_.find('Q');
    if (open_it != system_events_.end() && h.timestamp_ns >= open_it->second &&
        h.timestamp_ns - open_it->second < 1'000'000'000ULL)
        ++e_not_front_first_second_;
    std::uint64_t position = 1, ahead_shares = 0;
    int front_origin = -1;
    bool reached = false;
    b.for_each_order(o.side, [&](Price p, OrderId id, Qty q) {
        if (reached || p != o.price) return;
        if (id == ref) {
            reached = true;
            return;
        }
        ++position;
        ahead_shares += q;
        const auto it = ledger_.find(id);
        const int org = it != ledger_.end() ? it->second.origin : 0;
        ++ahead_by_origin_[org];
        if (front_origin < 0) front_origin = org;
    });
    ++e_not_front_position_[position == 2 ? 0 : position == 3 ? 1 : position <= 10 ? 2 : 3];
    if (e_not_front_examples_.size() < kMaxExamples) {
        static constexpr const char* names[] = {"add", "U same px fewer sh", "U same px same/more sh", "U new px"};
        e_not_front_examples_.push_back(
            {h.timestamp_ns, h.stock_locate, ref,
             "position " + std::to_string(position) + ", " + std::to_string(ahead_shares) + " sh ahead; executed order: " +
                 names[origin] + "; front order: " + (front_origin >= 0 ? names[front_origin] : "?")});
    }
}

void ReplayValidator::on(const itch::CrossTrade& m) {
    tick(m.header);
    cross_marks_.push_back({m.header.stock_locate, m.header.timestamp_ns, m.cross_type});
    auto& c = crosses_[m.cross_type];
    ++c.first;
    c.second += m.shares;
}

void ReplayValidator::on(const itch::BrokenTrade& m) {
    tick(m.header);
    ++broken_trades_;
}

// ---- End of day ----------------------------------------------------------------------

void ReplayValidator::finish() {
    if (finished_) return;
    finished_ = true;
    for (std::size_t L = 0; L < episode_active_.size(); ++L)
        if (episode_active_[L]) close_episode(static_cast<std::uint16_t>(L), last_ts_, true);

    // Post-hoc: is each non-printable C within 1 s of a cross in that stock?
    std::sort(cross_marks_.begin(), cross_marks_.end(),
              [](const CrossMark& a, const CrossMark& b) { return a.locate != b.locate ? a.locate < b.locate : a.ts < b.ts; });
    for (const auto& [t, L, rel] : nonprintable_execs_) {
        const auto lo = std::lower_bound(cross_marks_.begin(), cross_marks_.end(), CrossMark{L, t, 0},
                                         [](const CrossMark& a, const CrossMark& b) {
                                             return a.locate != b.locate ? a.locate < b.locate : a.ts < b.ts;
                                         });
        const CrossMark* best = nullptr;
        std::uint64_t best_d = 0;
        for (auto it : {lo, lo == cross_marks_.begin() ? cross_marks_.end() : std::prev(lo)}) {
            if (it == cross_marks_.end() || it->locate != L) continue;
            const std::uint64_t d = it->ts > t ? it->ts - t : t - it->ts;
            if (best == nullptr || d < best_d) {
                best = &*it;
                best_d = d;
            }
        }
        const bool near = best != nullptr && best_d <= 1'000'000'000ULL;
        ++np_cross_by_rel_[near ? 1 : 0][rel];
        if (!near) {
            ++np_near_cross_[4];
        } else {
            ++np_near_cross_[best->type == 'O' ? 0 : best->type == 'C' ? 1 : best->type == 'H' ? 2 : 3];
        }
    }

    for (const auto& [ref, e] : ledger_) {
        const FastBook* b = books_.find(e.locate);
        const auto o = b != nullptr ? b->find(ref) : std::nullopt;
        const itch::Header h{e.locate, 0, last_ts_};
        if (!o) {
            if (auto* ex = record(Issue::ledger_mismatch, h, ref)) ex->detail = "open in ledger at end of day, not in engine";
        } else if (o->qty != e.open) {
            if (auto* ex = record(Issue::ledger_mismatch, h, ref))
                ex->detail = "end of day: ledger " + std::to_string(e.open) + " sh, engine " + std::to_string(o->qty);
        }
        SymbolStats& st = stats(e.locate);
        st.eod_ledger_shares += e.open;
        ++st.eod_ledger_orders;
    }

    books_.for_each([&](std::uint16_t L, const FastBook& b) {
        SymbolStats& st = stats(L);
        for (Side s : {Side::buy, Side::sell}) b.for_each_level(s, [&](const LevelView& lv) { st.eod_book_shares += lv.shares; });
        st.eod_book_orders = b.order_count();
        eod_orders_ += b.order_count();
        eod_shares_ += st.eod_book_shares;
        if (b.order_count() > 0) ++eod_books_nonempty_;
        if (trading_state_[L] == 0 && st.book_messages > 0) ++books_without_trading_action_;
    });

    for (std::size_t L = 0; L < symbols_.size(); ++L) {
        if (!has_symbol_stats_[L]) continue;
        const SymbolStats& st = symbols_[L];
        if (st.book_messages == 0 && st.eod_book_orders == 0) continue;
        ++symbols_with_activity_;
        const std::uint64_t out =
            st.executed_shares + st.cancelled_shares + st.deleted_shares + st.replaced_shares + st.eod_ledger_shares;
        const bool ok = st.added_shares == out && st.eod_ledger_shares == st.eod_book_shares &&
                        st.eod_ledger_orders == st.eod_book_orders;
        if (ok) {
            ++symbols_reconciled_;
        } else {
            const itch::Header h{static_cast<std::uint16_t>(L), 0, last_ts_};
            if (auto* e = record(Issue::reconciliation_failure, h, 0))
                e->detail = "added " + std::to_string(st.added_shares) + " vs accounted " + std::to_string(out) +
                            "; EOD ledger " + std::to_string(st.eod_ledger_shares) + " sh / book " +
                            std::to_string(st.eod_book_shares) + " sh";
        }
    }
}

// ---- Reports ---------------------------------------------------------------------------

void ReplayValidator::write_peaks_csv(std::ostream& out) const {
    out << "locate,symbol,peak_orders,peak_bid_levels,peak_ask_levels,book_messages,adds,executions,eod_orders\n";
    for (std::size_t L = 0; L < symbols_.size(); ++L) {
        if (!has_symbol_stats_[L]) continue;
        const SymbolStats& s = symbols_[L];
        if (s.book_messages == 0) continue;
        out << L << ',' << locates_.symbol(static_cast<std::uint16_t>(L)) << ',' << s.peak_orders << ','
            << s.peak_bid_levels << ',' << s.peak_ask_levels << ',' << s.book_messages << ',' << s.adds << ','
            << s.executions << ',' << s.eod_book_orders << '\n';
    }
}

void ReplayValidator::write_markdown(std::ostream& md, const std::string& input_name,
                                     const std::vector<std::string>& focus) const {
    auto sym = [&](std::uint16_t L) {
        const auto s = locates_.symbol(L);
        return s.empty() ? "locate " + std::to_string(L) : std::string(s);
    };
    std::uint64_t book_msgs = 0;
    for (char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U'}) book_msgs += book_messages_by_type_[static_cast<unsigned char>(t)];

    md << "# Replay validation — " << input_name << "\n\n";
    md << "Generated by `tools/itch_validate` (method: `PROGRESS.md`, Rule D). Engine: `FastBook`, one book per\n"
          "stock locate. Every count below is measured; nothing is estimated or dropped.\n\n";

    md << "## Summary\n\n| | |\n|---|---|\n";
    md << "| Messages read | " << num(messages_) << " |\n";
    md << "| Unknown type / wrong length | " << unknown_messages_ << " / " << bad_length_messages_ << " |\n";
    md << "| Book messages (A F E C X D U) | " << num(book_msgs) << " |\n";
    for (char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U'})
        md << "| &nbsp;&nbsp;" << t << " | " << num(book_messages_by_type_[static_cast<unsigned char>(t)]) << " |\n";
    md << "| Exceptions recorded (all kinds) | " << num(issue_total()) << " |\n";
    md << "| Stocks with book activity | " << num(symbols_with_activity_) << " |\n";
    md << "| Stocks whose shares reconcile exactly | " << num(symbols_reconciled_) << " |\n";
    md << "| Peak live orders, whole market | " << num(ledger_peak_) << " |\n";
    md << "| Orders left in the end-of-day book | " << num(eod_orders_) << " (" << num(eod_shares_) << " shares, "
       << num(eod_books_nonempty_) << " stocks) |\n\n";

    md << "System events: ";
    bool first = true;
    for (const auto& [code, t] : system_events_) {
        md << (first ? "" : ", ") << "`" << code << "` " << ts(t);
        first = false;
    }
    md << ". Continuous trading = between `Q` and `M`, while the stock's latest trading action is `T` "
          "(stocks with no `H` message are treated as trading; "
       << num(books_without_trading_action_) << " stocks with book activity had none).\n\n";

    md << "## Exceptions by kind\n\n| Kind | Count | Meaning |\n|---|---:|---|\n";
    for (std::size_t i = 0; i < kIssueCount; ++i)
        md << "| `" << to_string(static_cast<Issue>(i)) << "` | " << num(issues_[i].count) << " | "
           << describe(static_cast<Issue>(i)) << " |\n";
    md << "\n";
    for (std::size_t i = 0; i < kIssueCount; ++i) {
        const IssueLog& log = issues_[i];
        if (log.count == 0) continue;
        md << "### `" << to_string(static_cast<Issue>(i)) << "` — " << num(log.count) << "\n\n";
        md << "First " << log.examples.size() << " in file order:\n\n| Time | Stock | Ref | Detail |\n|---|---|---:|---|\n";
        for (const auto& e : log.examples)
            md << "| " << ts(e.ts) << " | " << sym(e.locate) << " | " << e.ref << " | " << e.detail << " |\n";
        std::vector<std::pair<std::uint64_t, std::uint16_t>> top;
        for (const auto& [L, c] : log.by_locate) top.push_back({c, L});
        std::sort(top.rbegin(), top.rend());
        md << "\nStocks with most (" << log.by_locate.size() << " stocks in total): ";
        for (std::size_t k = 0; k < std::min<std::size_t>(10, top.size()); ++k)
            md << (k ? ", " : "") << sym(top[k].second) << " " << num(top[k].first);
        md << "\n\n";
    }

    md << "## 1. Executions\n\n| | E (no price) | C (with price) |\n|---|---:|---:|\n";
    md << "| Messages | " << num(exec_count_[0]) << " | " << num(exec_count_[1]) << " |\n";
    md << "| Shares | " << num(exec_shares_[0]) << " | " << num(exec_shares_[1]) << " |\n";
    md << "| Executed order was first in its queue | " << num(exec_at_front_[0]) << " (" << pct(exec_at_front_[0], exec_count_[0])
       << ") | " << num(exec_at_front_[1]) << " (" << pct(exec_at_front_[1], exec_count_[1]) << ") |\n\n";
    md << "`C` execution price vs the order's resting price (\"better\" = better for the resting order):\n\n";
    md << "| Printable | Equal | Better | Worse | Not at best price on its side | During market hours |\n";
    md << "|---|---:|---:|---:|---:|---:|\n";
    for (int pr = 0; pr < 2; ++pr) {
        md << "| " << (pr == 0 ? "Y" : "N");
        for (int rel = 0; rel < 3; ++rel)
            md << " | " << num(priced_.count[pr][rel]) << " (" << num(priced_.shares[pr][rel]) << " sh)";
        md << " | " << num(priced_.not_at_best[pr]) << " | " << num(priced_.during_market_hours[pr]) << " |\n";
    }
    md << "\n`X` messages that cancelled every remaining share (order removed): " << num(cancel_all_remaining_) << ".\n\n";

    md << "## 2. Crossed and locked books\n\nChecked after every book message for that stock.\n\n";
    md << "| Kind | When | Episodes | Messages while in that state | Ended by a message with the same timestamp "
          "| 0 ns | < 1 us | < 1 ms | < 1 s | >= 1 s |\n|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (int c = 1; c >= 0; --c)
        for (int cont = 1; cont >= 0; --cont) {
            const CrossStats& cs = cross_[c][cont];
            md << "| " << (c ? "crossed" : "locked") << " | " << (cont ? "continuous trading" : "outside") << " | "
               << num(cs.episodes) << " | " << num(cs.messages) << " | " << num(cs.same_timestamp);
            for (auto v : cs.duration) md << " | " << num(v);
            md << " |\n";
        }
    md << "\n";
    for (int c = 1; c >= 0; --c)
        for (int cont = 1; cont >= 0; --cont) {
            const CrossStats& cs = cross_[c][cont];
            if (cs.episodes == 0) continue;
            md << "First " << cs.examples.size() << " " << (c ? "crossed" : "locked") << " episodes "
               << (cont ? "during continuous trading" : "outside continuous trading") << ":\n\n";
            md << "| Stock | State | Start | End | Messages | Bid | Ask |\n|---|---|---|---|---:|---:|---:|\n";
            for (const auto& e : cs.examples)
                md << "| " << sym(e.locate) << " | " << (e.trading_state ? std::string(1, e.trading_state) : "-") << " | "
                   << ts(e.start_ts) << " | " << ts(e.end_ts) << (e.open_at_end ? " (still open)" : "") << " | "
                   << e.messages << " | " << px(e.bid) << " | " << px(e.ask) << " |\n";
            md << "\n";
        }

    md << "## 3. Reconciliation\n\n";
    SymbolStats total;
    for (std::size_t L = 0; L < symbols_.size(); ++L) {
        if (!has_symbol_stats_[L]) continue;
        const auto& s = symbols_[L];
        total.added_shares += s.added_shares;
        total.executed_shares += s.executed_shares;
        total.cancelled_shares += s.cancelled_shares;
        total.deleted_shares += s.deleted_shares;
        total.replaced_shares += s.replaced_shares;
        total.eod_ledger_shares += s.eod_ledger_shares;
        total.eod_book_shares += s.eod_book_shares;
    }
    md << "Shares, whole market (ledger kept independently of the engine):\n\n| | Shares |\n|---|---:|\n";
    md << "| Added (A, F, and the new size of every U) | " << num(total.added_shares) << " |\n";
    md << "| Executed (E, C) | " << num(total.executed_shares) << " |\n";
    md << "| Cancelled (X) | " << num(total.cancelled_shares) << " |\n";
    md << "| Still open when deleted (D) | " << num(total.deleted_shares) << " |\n";
    md << "| Still open when replaced (U) | " << num(total.replaced_shares) << " |\n";
    md << "| Open at end of day, ledger | " << num(total.eod_ledger_shares) << " |\n";
    md << "| Open at end of day, engine's book | " << num(total.eod_book_shares) << " |\n";
    md << "| Added - all the rest | "
       << static_cast<std::int64_t>(total.added_shares - total.executed_shares - total.cancelled_shares -
                                    total.deleted_shares - total.replaced_shares - total.eod_ledger_shares)
       << " |\n\n";

    md << "## 4. Messages that do not change the visible book\n\n| | Count | Shares |\n|---|---:|---:|\n";
    md << "| P trade (non-cross: non-displayed orders) | " << num(trades_) << " | " << num(trade_shares_) << " |\n";
    md << "| &nbsp;&nbsp;of which order reference != 0 | " << num(trades_nonzero_ref_) << " | |\n";
    for (const auto& [type, c] : crosses_)
        md << "| Q cross trade, type `" << type << "` | " << num(c.first) << " | " << num(c.second) << " |\n";
    md << "| B broken trade | " << num(broken_trades_) << " | |\n";
    for (const auto& [state, c] : trading_actions_)
        md << "| H trading action, state `" << state << "` | " << num(c) << " | |\n";
    md << "\n";
    if (!halts_.empty()) {
        md << "First " << halts_.size() << " trading actions with a state other than `T`:\n\n"
           << "| Time | Stock | Detail |\n|---|---|---|\n";
        for (const auto& e : halts_) md << "| " << ts(e.ts) << " | " << sym(e.locate) << " | " << e.detail << " |\n";
        md << "\n";
    }

    md << "## 5. Book sizes (used to pre-size the benchmark and to justify the design)\n\n";
    std::size_t max_orders = 0, max_bid = 0, max_ask = 0;
    std::uint16_t at_orders = 0, at_bid = 0, at_ask = 0;
    for (std::size_t L = 0; L < symbols_.size(); ++L) {
        if (!has_symbol_stats_[L]) continue;
        const auto& s = symbols_[L];
        if (s.peak_orders > max_orders) { max_orders = s.peak_orders; at_orders = static_cast<std::uint16_t>(L); }
        if (s.peak_bid_levels > max_bid) { max_bid = s.peak_bid_levels; at_bid = static_cast<std::uint16_t>(L); }
        if (s.peak_ask_levels > max_ask) { max_ask = s.peak_ask_levels; at_ask = static_cast<std::uint16_t>(L); }
    }
    md << "- Largest book: " << num(max_orders) << " live orders (" << sym(at_orders) << ").\n";
    md << "- Most bid levels at once: " << num(max_bid) << " (" << sym(at_bid) << "); most ask levels: " << num(max_ask)
       << " (" << sym(at_ask) << ").\n\n";
    md << "| Stock | Book msgs | Adds | Executions | Peak orders | Peak bid levels | Peak ask levels | Lowest price "
          "| Highest price | EOD orders |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& f : focus) {
        const auto L = locates_.find(f);
        if (!L || !has_symbol_stats_[*L]) continue;
        const auto& s = symbols_[*L];
        md << "| " << f << " | " << num(s.book_messages) << " | " << num(s.adds) << " | " << num(s.executions) << " | "
           << num(s.peak_orders) << " | " << num(s.peak_bid_levels) << " | " << num(s.peak_ask_levels) << " | $"
           << px(s.min_price) << " | $" << px(s.max_price) << " | " << num(s.eod_book_orders) << " |\n";
    }
    md << "\nTimestamps going backwards: " << num(issues_[static_cast<std::size_t>(Issue::timestamp_backwards)].count)
       << ".\n\n";

    md << "## 6. Post-hoc breakdowns (added after the first run to explain its findings; descriptive, not pass/fail)\n\n";
    md << "`C` executions whose price differs from the resting price, by size of the difference "
          "(units of $0.0001):\n\n| Printable | 1-49 | 50 (half a cent) | 51-99 | 100 (one cent) | 101-999 | >= 1000 |\n"
          "|---|---:|---:|---:|---:|---:|---:|\n";
    for (int pr = 0; pr < 2; ++pr) {
        md << "| " << (pr == 0 ? "Y" : "N");
        for (auto v : c_gap_[pr]) md << " | " << num(v);
        md << " |\n";
    }
    std::uint64_t np_total = 0;
    for (auto v : np_near_cross_) np_total += v;
    md << "\nNon-printable `C` executions (" << num(np_total) << ") by the nearest `Q` cross of the same stock within 1 s: "
       << "opening " << num(np_near_cross_[0]) << ", closing " << num(np_near_cross_[1]) << ", halt/IPO "
       << num(np_near_cross_[2]) << ", other " << num(np_near_cross_[3]) << ", no cross within 1 s " << num(np_near_cross_[4])
       << ".\n\n| Non-printable `C` | Equal price | Better for resting order | Worse for resting order |\n"
          "|---|---:|---:|---:|\n";
    for (int near = 1; near >= 0; --near)
        md << "| " << (near ? "within 1 s of a cross" : "no cross within 1 s") << " | " << num(np_cross_by_rel_[near][0])
           << " | " << num(np_cross_by_rel_[near][1]) << " | " << num(np_cross_by_rel_[near][2]) << " |\n";
    md << "\n";
    static constexpr const char* origin_names[] = {"A/F add", "U, same price, fewer shares", "U, same price, same or more shares",
                                                   "U, new price"};
    std::uint64_t nf = 0, fr = 0;
    for (int o = 0; o < kOrigins; ++o) {
        nf += e_by_origin_[0][o];
        fr += e_by_origin_[1][o];
    }
    md << "`E` executions by how the executed order was created, split by whether it was first in its queue:\n\n"
          "| Executed order created by | Not first in queue | Share of those | First in queue | Share of those |\n"
          "|---|---:|---:|---:|---:|\n";
    for (int o = 0; o < kOrigins; ++o)
        md << "| " << origin_names[o] << " | " << num(e_by_origin_[0][o]) << " | " << pct(e_by_origin_[0][o], nf) << " | "
           << num(e_by_origin_[1][o]) << " | " << pct(e_by_origin_[1][o], fr) << " |\n";
    md << "\nNot-first `E` executions: " << num(e_not_front_market_hours_) << " during market hours; queue position 2: "
       << num(e_not_front_position_[0]) << ", 3: " << num(e_not_front_position_[1]) << ", 4-10: " << num(e_not_front_position_[2])
       << ", >10: " << num(e_not_front_position_[3]) << "; within 1 s after the market-open event: "
       << num(e_not_front_first_second_) << ". By hour (ET): ";
    for (int hr = 0; hr < 24; ++hr)
        if (e_not_front_by_hour_[hr]) md << hr << "h " << num(e_not_front_by_hour_[hr]) << "; ";
    md << "orders ahead of them, by origin: ";
    for (int o = 0; o < kOrigins; ++o) md << (o ? ", " : "") << origin_names[o] << " " << num(ahead_by_origin_[o]);
    md << ".\n\n| Time | Stock | Ref | Detail |\n|---|---|---:|---|\n";
    for (const auto& e : e_not_front_examples_)
        md << "| " << ts(e.ts) << " | " << sym(e.locate) << " | " << e.ref << " | " << e.detail << " |\n";
}

}  // namespace tickforge::book
