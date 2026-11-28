#include "tickforge/book/queue_jumps.hpp"

#include <algorithm>
#include <cstdio>
#include <ostream>
#include <string>

namespace tickforge::book {

const char* to_string(Fate f) noexcept {
    switch (f) {
        case Fate::none: return "none";
        case Fate::executed_e: return "executed (E)";
        case Fate::executed_c: return "executed (C)";
        case Fate::cancelled: return "partially cancelled (X)";
        case Fate::deleted: return "deleted (D)";
        case Fate::replaced: return "replaced (U)";
        case Fate::end_of_day: return "still in the book at end of file";
    }
    return "?";
}

const char* to_string(JumpClass c) noexcept {
    switch (c) {
        case JumpClass::all_ahead_executed_same_ts: return "every skipped order also executed at the same timestamp";
        case JumpClass::all_ahead_gone_same_ts: return "every skipped order left at the same timestamp, some by X/D/U";
        case JumpClass::some_ahead_remain: return "a skipped order was still resting after that timestamp";
    }
    return "?";
}

namespace {

std::uint32_t pack_mpid(const itch::Mpid& m) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(m.chars[0])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(m.chars[1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(m.chars[2])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(m.chars[3]));
}

std::string mpid_str(std::uint32_t m) {
    std::string s;
    for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>((m >> (8 * i)) & 0xFF));
    return s;
}

std::string fmt_ts(std::uint64_t ns) {
    char buf[32];
    const std::uint64_t s = ns / 1'000'000'000ULL;
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>((s / 60) % 60), static_cast<unsigned long long>(s % 60),
                  static_cast<unsigned long long>(ns % 1'000'000'000ULL));
    return buf;
}

std::string num(std::uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

std::string pct(std::uint64_t a, std::uint64_t b) {
    if (b == 0) return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f%%", 100.0 * static_cast<double>(a) / static_cast<double>(b));
    return buf;
}

std::string price_str(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld.%04lld", static_cast<long long>(p / 10000), static_cast<long long>(p % 10000));
    return buf;
}

}  // namespace

QueueJumpAnalyzer::QueueJumpAnalyzer() : max_ref_(65536, 0), ref_clock_(65536) { info_.reserve(1 << 22); }

void QueueJumpAnalyzer::tick(std::uint64_t ts) {
    if (ts != cur_ts_) {
        end_timestamp();
        cur_ts_ = ts;
    }
}

void QueueJumpAnalyzer::end_timestamp() {
    for (std::uint32_t ji : open_jumps_) {
        QueueJump& j = jumps_[ji];
        j.in_cross = std::find(cross_locates_.begin(), cross_locates_.end(), j.locate) != cross_locates_.end();
        j.hidden_trade_same_ts =
            std::find(hidden_locates_.begin(), hidden_locates_.end(), j.locate) != hidden_locates_.end();
        bool all_gone = true, all_exec = true;
        for (SkippedOrder& s : j.ahead) {
            s.gone_by_execution = s.gone_same_ts && !s.other_same_ts;
            if (!s.gone_same_ts) all_gone = false;
            if (!s.gone_by_execution) all_exec = false;
        }
        j.cls = !all_gone ? JumpClass::some_ahead_remain
                          : (all_exec ? JumpClass::all_ahead_executed_same_ts : JumpClass::all_ahead_gone_same_ts);
        j.classified = true;
    }
    open_jumps_.clear();
    cross_locates_.clear();
    hidden_locates_.clear();
}

void QueueJumpAnalyzer::on(const itch::SystemEvent& m) {
    tick(m.header.timestamp_ns);
    if (m.event_code == 'Q') {
        market_open_ = true;
        open_ts_ = m.header.timestamp_ns;
    } else if (m.event_code == 'M') {
        market_open_ = false;
    }
}

void QueueJumpAnalyzer::on(const itch::StockDirectory& m) {
    tick(m.header.timestamp_ns);
    locates_.add(m);
}

void QueueJumpAnalyzer::add(const itch::Header& h, OrderId ref, char side, Qty shares, Price price, char kind,
                            std::uint32_t mpid) {
    FastBook& b = books_.get(h.stock_locate);
    const Side s = side_from_itch(side);
    OrderInfo info;
    info.locate = h.stock_locate;
    info.kind = kind;
    info.mpid = mpid;
    info.add_ts = h.timestamp_ns;
    info.added_shares = shares;
    if (const auto opp = b.best(opposite(s))) info.opposite_gap = s == Side::buy ? opp->price - price : price - opp->price;
    note_ref(info, ref, h.timestamp_ns);
    if (b.add(ref, s, price, shares) == Status::ok) info_[ref] = info;
}

void QueueJumpAnalyzer::note_ref(OrderInfo& info, OrderId ref, std::uint64_t ts) {
    ++adds_;
    const std::uint16_t L = info.locate;
    auto& clock = ref_clock_[L];
    if (ref > max_ref_[L]) {
        max_ref_[L] = ref;
        if (clock.empty() || ts - clock.back().second >= 1'000'000) {
            clock.emplace_back(ref, ts);
        } else {
            clock.back().first = ref;  // same millisecond: extend the entry
        }
        return;
    }
    // Late: this stock already published a larger reference. When (to 1 ms)?
    const auto it = std::upper_bound(clock.begin(), clock.end(), ref,
                                     [](std::uint64_t r, const auto& e) { return r < e.first; });
    info.late = true;
    info.late_by_ns = it != clock.end() && ts > it->second ? ts - it->second : 0;
    ++late_adds_;
    if (late_first_ts_ == 0) late_first_ts_ = ts;
    late_last_ts_ = ts;
    if (open_ts_ != 0 && ts >= open_ts_ && ts - open_ts_ < 1'000'000'000ULL) ++late_first_second_;
    ++late_by_hour_[(ts / 3'600'000'000'000ULL) % 24];
    const std::uint64_t d = info.late_by_ns;
    ++late_delay_[d < 1'000'000 ? 0 : d < 1'000'000'000 ? 1 : d < 60'000'000'000ULL ? 2 : d < 600'000'000'000ULL ? 3
                  : d < 3'600'000'000'000ULL ? 4 : 5];
}

void QueueJumpAnalyzer::on(const itch::AddOrder& m) {
    tick(m.header.timestamp_ns);
    add(m.header, m.order_reference_number, m.side, m.shares, m.price, 'A', 0);
}

void QueueJumpAnalyzer::on(const itch::AddOrderMpid& m) {
    tick(m.header.timestamp_ns);
    add(m.header, m.order_reference_number, m.side, m.shares, m.price, 'F', pack_mpid(m.attribution));
}

void QueueJumpAnalyzer::touch(const itch::Header& h, OrderId ref, Fate kind, Qty shares, Price c_price) {
    auto wit = watch_.find(ref);
    if (wit == watch_.end()) return;
    const FastBook* b = books_.find(h.stock_locate);
    const auto o = b ? b->find(ref) : std::nullopt;
    if (!o) return;
    const bool gone = kind == Fate::deleted || kind == Fate::replaced || shares >= o->qty;
    std::vector<Watch>& ws = wit->second;
    std::size_t keep = 0;
    for (std::size_t i = 0; i < ws.size(); ++i) {
        QueueJump& j = jumps_[ws[i].jump];
        SkippedOrder& s = j.ahead[ws[i].idx];
        bool done = false;
        if (!j.classified) {  // same timestamp as the jump
            if (kind != Fate::executed_e && kind != Fate::executed_c) s.other_same_ts = true;
            if (gone) {
                s.gone_same_ts = true;
                done = true;
            }
        } else if (!s.gone_same_ts && s.next == Fate::none) {
            s.next = kind;
            s.next_dt_ns = h.timestamp_ns - j.ts;
            if (kind == Fate::executed_c) {
                const bool more_aggr = o->side == Side::buy ? c_price > o->price : c_price < o->price;
                s.next_c_rel = c_price == o->price ? 0 : (more_aggr ? 1 : 2);
            }
            done = true;
        } else {
            done = true;
        }
        if (!done) ws[keep++] = ws[i];
    }
    ws.resize(keep);
    if (ws.empty()) watch_.erase(wit);
}

void QueueJumpAnalyzer::forget(OrderId ref) { info_.erase(ref); }

void QueueJumpAnalyzer::on(const itch::OrderExecuted& m) {
    tick(m.header.timestamp_ns);
    FastBook& b = books_.get(m.header.stock_locate);
    const OrderId ref = m.order_reference_number;
    const auto o = b.find(ref);
    if (!o) return;
    ++e_total_;
    const auto iit = info_.find(ref);
    const OrderInfo inf = iit != info_.end() ? iit->second : OrderInfo{};
    {
        bool min_ref = true, late_smaller = false;
        b.for_each_order_at(o->side, o->price, [&](OrderId id, Qty) {
            if (id < ref) {
                min_ref = false;
                const auto it = info_.find(id);
                if (it != info_.end() && it->second.late) late_smaller = true;
            }
        });
        ++e_order_[o->at_front ? 1 : 0][min_ref ? 1 : 0];
        if (!min_ref && late_smaller) ++e_not_min_late_ahead_;
    }
    if (o->at_front) {
        ++e_front_;
        if (inf.added_shares < 100) ++e_front_odd_lot_;
        if (inf.kind == 'F') ++e_front_mpid_;
        if (open_ts_ == 0 || inf.add_ts < open_ts_) ++e_front_premarket_add_;
    } else {
        QueueJump j;
        j.ts = m.header.timestamp_ns;
        j.locate = m.header.stock_locate;
        j.ref = ref;
        j.side = o->side;
        j.price = o->price;
        j.executed = m.executed_shares;
        j.fully_executed = m.executed_shares >= o->qty;
        j.market_hours = market_open_;
        j.jumper = inf;
        std::uint32_t pos = 1;
        bool reached = false;
        b.for_each_order_at(o->side, o->price, [&](OrderId id, Qty q) {
            if (reached) return;
            if (id == ref) {
                reached = true;
                return;
            }
            ++pos;
            j.ahead_shares += q;
            SkippedOrder s;
            s.id = id;
            s.shares = q;
            const auto it = info_.find(id);
            if (it != info_.end()) s.info = it->second;
            j.ahead.push_back(s);
        });
        j.position = pos;
        const auto ji = static_cast<std::uint32_t>(jumps_.size());
        for (std::uint32_t k = 0; k < j.ahead.size(); ++k) watch_[j.ahead[k].id].push_back({ji, k});
        jumps_.push_back(std::move(j));
        open_jumps_.push_back(ji);
    }
    touch(m.header, ref, Fate::executed_e, m.executed_shares, 0);
    const auto r = b.execute(ref, m.executed_shares);
    if (r.status == Status::ok) {
        if (r.remaining == 0) {
            forget(ref);
        } else if (iit != info_.end()) {
            ++iit->second.partial_execs;
        }
    }
}

void QueueJumpAnalyzer::on(const itch::OrderExecutedWithPrice& m) {
    tick(m.header.timestamp_ns);
    FastBook& b = books_.get(m.header.stock_locate);
    touch(m.header, m.order_reference_number, Fate::executed_c, m.executed_shares, m.execution_price);
    const auto r = b.execute(m.order_reference_number, m.executed_shares);
    if (r.status == Status::ok) {
        if (r.remaining == 0) {
            forget(m.order_reference_number);
        } else if (auto it = info_.find(m.order_reference_number); it != info_.end()) {
            ++it->second.partial_execs;
        }
    }
}

void QueueJumpAnalyzer::on(const itch::OrderCancel& m) {
    tick(m.header.timestamp_ns);
    FastBook& b = books_.get(m.header.stock_locate);
    touch(m.header, m.order_reference_number, Fate::cancelled, m.cancelled_shares, 0);
    const auto o = b.find(m.order_reference_number);
    if (b.cancel(m.order_reference_number, m.cancelled_shares) == Status::ok) {
        if (o && o->qty == m.cancelled_shares) {
            forget(m.order_reference_number);
        } else if (auto it = info_.find(m.order_reference_number); it != info_.end()) {
            ++it->second.partial_cancels;
        }
    }
}

void QueueJumpAnalyzer::on(const itch::OrderDelete& m) {
    tick(m.header.timestamp_ns);
    FastBook& b = books_.get(m.header.stock_locate);
    touch(m.header, m.order_reference_number, Fate::deleted, 0, 0);
    if (b.remove(m.order_reference_number) == Status::ok) forget(m.order_reference_number);
}

void QueueJumpAnalyzer::on(const itch::OrderReplace& m) {
    tick(m.header.timestamp_ns);
    FastBook& b = books_.get(m.header.stock_locate);
    touch(m.header, m.original_order_reference_number, Fate::replaced, 0, 0);
    const auto o = b.find(m.original_order_reference_number);
    if (!o) return;
    OrderInfo info;
    info.locate = m.header.stock_locate;
    info.kind = 'U';
    info.add_ts = m.header.timestamp_ns;
    info.added_shares = m.shares;
    if (const auto opp = b.best(opposite(o->side)))
        info.opposite_gap = o->side == Side::buy ? opp->price - static_cast<Price>(m.price)
                                                 : static_cast<Price>(m.price) - opp->price;
    note_ref(info, m.new_order_reference_number, m.header.timestamp_ns);
    if (b.replace(m.original_order_reference_number, m.new_order_reference_number, m.price, m.shares) == Status::ok) {
        forget(m.original_order_reference_number);
        info_[m.new_order_reference_number] = info;
    }
}

void QueueJumpAnalyzer::on(const itch::Trade& m) {
    tick(m.header.timestamp_ns);
    hidden_locates_.push_back(m.header.stock_locate);
}

void QueueJumpAnalyzer::on(const itch::CrossTrade& m) {
    tick(m.header.timestamp_ns);
    cross_locates_.push_back(m.header.stock_locate);
}

void QueueJumpAnalyzer::finish() {
    if (finished_) return;
    end_timestamp();
    for (auto& [id, ws] : watch_) {
        for (const Watch& w : ws) {
            SkippedOrder& s = jumps_[w.jump].ahead[w.idx];
            if (!s.gone_same_ts && s.next == Fate::none) s.next = Fate::end_of_day;
        }
    }
    watch_.clear();
    finished_ = true;
}

void QueueJumpAnalyzer::write_markdown(std::ostream& md, const std::string& input_name) const {
    md << "# Queue jumps: `E` executions of an order that was not first in its queue\n\n";
    md << "Input: `" << input_name << "`. Generated by `tools/itch_queue_jumps` "
       << "(`include/tickforge/book/queue_jumps.hpp`). Analysis only: it looks ahead in the feed.\n\n";
    md << "| | Count |\n|---|---:|\n";
    md << "| `E` executions | " << num(e_total_) << " |\n";
    md << "| executed order was first in its queue | " << num(e_front_) << " (" << pct(e_front_, e_total_) << ") |\n";
    md << "| executed order was **not** first (queue jumps) | " << num(jumps_.size()) << " ("
       << pct(jumps_.size(), e_total_) << ") |\n\n";

    md << "## 0. Arrival order vs reference-number order\n\n";
    md << "An add is \"late\" if the same stock already published a larger order reference number before it "
          "(measured per stock, to 1 ms). For every `E`: was the executed order first in "
          "*arrival* order at its level (the order our book queues by), and did it have the *smallest reference "
          "number* at its level?\n\n";
    md << "| `E` executions | Smallest reference at its level | Not the smallest |\n|---|---:|---:|\n";
    md << "| first in arrival order | " << num(e_order_[1][1]) << " | " << num(e_order_[1][0]) << " |\n";
    md << "| not first in arrival order (queue jumps) | " << num(e_order_[0][1]) << " | " << num(e_order_[0][0])
       << " |\n\n";
    md << "Of the `E` whose order did not have the smallest reference at its level, " << num(e_not_min_late_ahead_)
       << " had a late add among the smaller references.\n\n";
    md << "Late adds: " << num(late_adds_) << " of " << num(adds_) << " adds (A, F and the new reference of U). "
       << "Delay from the first larger reference being published to the late add: < 1 ms " << num(late_delay_[0])
       << "; < 1 s " << num(late_delay_[1]) << "; < 1 min " << num(late_delay_[2]) << "; < 10 min "
       << num(late_delay_[3]) << "; < 1 h " << num(late_delay_[4]) << "; >= 1 h " << num(late_delay_[5])
       << ". By hour (ET): ";
    for (int h = 0; h < 24; ++h)
        if (late_by_hour_[h]) md << h << "h " << num(late_by_hour_[h]) << "; ";
    md << "within 1 s after the market-open event (System Event `Q`" << (open_ts_ ? " at " + fmt_ts(open_ts_) : "")
       << "): " << num(late_first_second_) << "; first late add " << (late_adds_ ? fmt_ts(late_first_ts_) : "-")
       << ", last " << (late_adds_ ? fmt_ts(late_last_ts_) : "-") << ".\n\n";

    // 1. Classification.
    std::uint64_t cls[kJumpClasses][2] = {};   // [class][in cross]
    std::uint64_t cls_mh[kJumpClasses] = {};   // during market hours
    std::uint64_t cls_hidden[kJumpClasses] = {};
    std::uint64_t cls_hour[kJumpClasses][24] = {};
    std::uint64_t cls_shares[kJumpClasses] = {};
    for (const QueueJump& j : jumps_) {
        const auto c = static_cast<std::size_t>(j.cls);
        ++cls[c][j.in_cross ? 1 : 0];
        if (j.market_hours) ++cls_mh[c];
        if (j.hidden_trade_same_ts) ++cls_hidden[c];
        ++cls_hour[c][(j.ts / 3'600'000'000'000ULL) % 24];
        cls_shares[c] += j.executed;
    }
    md << "## 1. How each jump resolved by the end of its nanosecond timestamp\n\n";
    md << "A sweep by one aggressive order is published as a burst of `E` messages with one timestamp. "
          "\"Same timestamp\" below means messages carrying exactly the jump's timestamp.\n\n";
    md << "| Class | Jumps | Executed shares | With a `Q` cross for the stock at that timestamp | During market hours | "
          "With a hidden `P` trade for the stock at that timestamp |\n|---|---:|---:|---:|---:|---:|\n";
    for (std::size_t c = 0; c < kJumpClasses; ++c) {
        md << "| " << to_string(static_cast<JumpClass>(c)) << " | " << num(cls[c][0] + cls[c][1]) << " | "
           << num(cls_shares[c]) << " | " << num(cls[c][1]) << " | " << num(cls_mh[c]) << " | " << num(cls_hidden[c])
           << " |\n";
    }
    md << "\nBy hour (ET):\n\n| Class |";
    for (int h = 4; h <= 19; ++h) md << " " << h << "h |";
    md << "\n|---|";
    for (int h = 4; h <= 19; ++h) md << "---:|";
    md << "\n";
    for (std::size_t c = 0; c < kJumpClasses; ++c) {
        md << "| " << to_string(static_cast<JumpClass>(c)) << " |";
        for (int h = 4; h <= 19; ++h) md << " " << num(cls_hour[c][h]) << " |";
        md << "\n";
    }

    // 2. Skipped orders that stayed in the book.
    std::uint64_t fate[kFates] = {};
    std::uint64_t dt[6] = {};  // < 1 us, < 1 ms, < 1 s, < 60 s, >= 60 s, (end of day)
    std::uint64_t c_rel[3] = {};
    std::uint64_t stayed = 0, stayed_shares = 0;
    std::uint64_t remaining_per_jump[4] = {};  // 1, 2, 3-10, >10 skipped orders still resting
    for (const QueueJump& j : jumps_) {
        if (j.cls != JumpClass::some_ahead_remain) continue;
        std::uint64_t r = 0;
        for (const SkippedOrder& s : j.ahead) {
            if (s.gone_same_ts) continue;
            ++r;
            ++stayed;
            stayed_shares += s.shares;
            ++fate[static_cast<std::size_t>(s.next)];
            if (s.next == Fate::end_of_day) {
                ++dt[5];
            } else {
                const std::uint64_t d = s.next_dt_ns;
                ++dt[d < 1'000 ? 0 : d < 1'000'000 ? 1 : d < 1'000'000'000 ? 2 : d < 60'000'000'000ULL ? 3 : 4];
            }
            if (s.next == Fate::executed_c) ++c_rel[s.next_c_rel];
        }
        ++remaining_per_jump[r == 1 ? 0 : r == 2 ? 1 : r <= 10 ? 2 : 3];
    }
    md << "\n## 2. Skipped orders that were still resting after the jump's timestamp\n\n";
    md << "Skipped orders still resting: " << num(stayed) << " (" << num(stayed_shares)
       << " shares). Jumps by how many skipped orders stayed: 1: " << num(remaining_per_jump[0])
       << ", 2: " << num(remaining_per_jump[1]) << ", 3-10: " << num(remaining_per_jump[2])
       << ", >10: " << num(remaining_per_jump[3]) << ".\n\n";
    md << "Their next event:\n\n| Next event | Orders |\n|---|---:|\n";
    for (std::size_t f = 1; f < kFates; ++f) md << "| " << to_string(static_cast<Fate>(f)) << " | " << num(fate[f]) << " |\n";
    md << "\nTime from the jump to that next event: < 1 us: " << num(dt[0]) << "; < 1 ms: " << num(dt[1])
       << "; < 1 s: " << num(dt[2]) << "; < 60 s: " << num(dt[3]) << "; >= 60 s: " << num(dt[4])
       << "; no further event: " << num(dt[5]) << ".\n\n";
    md << "Of those next executed by `C`: at the displayed price " << num(c_rel[0])
       << ", at a more aggressive price than displayed " << num(c_rel[1]) << ", less aggressive " << num(c_rel[2])
       << ".\n";

    // 3. Attributes: skipped orders vs executed (jumping) orders vs a baseline.
    struct Attr {
        std::uint64_t n = 0, odd = 0, mpid = 0, replace = 0, premarket = 0, partial = 0, gap1 = 0, gapclose = 0;
        std::uint64_t same_mpid = 0, late = 0;
    };
    auto tally = [&](Attr& a, const OrderInfo& i) {
        ++a.n;
        if (i.added_shares < 100) ++a.odd;
        if (i.kind == 'F') ++a.mpid;
        if (i.kind == 'U') ++a.replace;
        if (open_ts_ == 0 || i.add_ts < open_ts_) ++a.premarket;
        if (i.partial_execs + i.partial_cancels > 0) ++a.partial;
        if (i.opposite_gap == 100) ++a.gap1;
        if (i.opposite_gap >= 0 && i.opposite_gap < 100) ++a.gapclose;
        if (i.late) ++a.late;
    };
    Attr stayed_a, gone_a, jumper_a, jumper_stay_a;
    for (const QueueJump& j : jumps_) {
        tally(jumper_a, j.jumper);
        if (j.cls == JumpClass::some_ahead_remain) tally(jumper_stay_a, j.jumper);
        for (const SkippedOrder& s : j.ahead) {
            Attr& a = s.gone_same_ts ? gone_a : stayed_a;
            tally(a, s.info);
            if (s.info.kind == 'F' && j.jumper.kind == 'F' && s.info.mpid == j.jumper.mpid) ++a.same_mpid;
        }
    }
    md << "\n## 3. What the orders look like\n\n";
    md << "\"Gap 1 cent\": when the order was added, the best price on the other side was exactly $0.01 away. "
          "\"Gap < 1 cent\": the other side was less than $0.01 away (sub-penny or zero).\n\n";
    md << "| Orders | Count | Odd lot (< 100 sh at add) | Attributed (F) | Created by U | Added before 09:30 | "
          "Partially executed/cancelled before | Gap 1 cent | Gap < 1 cent | Same MPID as the jumper | Late add |\n"
          "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    auto row = [&](const char* name, const Attr& a, bool show_same) {
        md << "| " << name << " | " << num(a.n) << " | " << pct(a.odd, a.n) << " | " << pct(a.mpid, a.n) << " | "
           << pct(a.replace, a.n) << " | " << pct(a.premarket, a.n) << " | " << pct(a.partial, a.n) << " | "
           << pct(a.gap1, a.n) << " | " << pct(a.gapclose, a.n) << " | "
           << (show_same ? num(a.same_mpid) : std::string("-")) << " | " << pct(a.late, a.n) << " |\n";
    };
    row("skipped, still resting after the timestamp", stayed_a, true);
    row("skipped, gone at the same timestamp", gone_a, true);
    row("executed (jumping) orders, all jumps", jumper_a, false);
    row("executed (jumping) orders, jumps with a skipped order still resting", jumper_stay_a, false);
    md << "\nBaseline, all `E` executions of an order that *was* first in its queue: " << num(e_front_)
       << "; odd lot " << pct(e_front_odd_lot_, e_front_) << ", attributed (F) " << pct(e_front_mpid_, e_front_)
       << ", added before 09:30 " << pct(e_front_premarket_add_, e_front_) << ".\n";

    // 4. Examples.
    md << "\n## 4. Examples\n\n";
    for (std::size_t c = 0; c < kJumpClasses; ++c) {
        md << "### " << to_string(static_cast<JumpClass>(c)) << "\n\n";
        md << "| Time | Stock | Side | Price | Ref | Position | Shares ahead | Executed | Cross | Skipped orders (ref, shares, kind, "
              "added, next event, after) |\n|---|---|---|---:|---:|---:|---:|---:|---|---|\n";
        int shown = 0;
        for (const QueueJump& j : jumps_) {
            if (static_cast<std::size_t>(j.cls) != c) continue;
            if (++shown > 15) break;
            md << "| " << fmt_ts(j.ts) << " | " << locates_.symbol(j.locate) << " | " << (j.side == Side::buy ? "B" : "S")
               << " | " << price_str(j.price) << " | " << j.ref << " | " << j.position << " | " << num(j.ahead_shares)
               << " | " << num(j.executed) << (j.fully_executed ? " (all)" : "") << " | " << (j.in_cross ? "yes" : "no")
               << " | ";
            int k = 0;
            for (const SkippedOrder& s : j.ahead) {
                if (++k > 4) {
                    md << "...";
                    break;
                }
                md << s.id << " " << s.shares << "sh " << s.info.kind;
                if (s.info.kind == 'F') md << "(" << mpid_str(s.info.mpid) << ")";
                md << " @" << fmt_ts(s.info.add_ts) << " ";
                if (s.gone_same_ts) {
                    md << (s.gone_by_execution ? "executed same ts" : "removed same ts");
                } else {
                    md << to_string(s.next);
                    if (s.next != Fate::end_of_day) md << " +" << num(s.next_dt_ns) << "ns";
                }
                md << "; ";
            }
            md << " |\n";
        }
        md << "\n";
    }
}

}  // namespace tickforge::book
