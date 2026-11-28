#include "tickforge/sim/mm_strategies.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tickforge::sim {

namespace {
constexpr std::uint64_t kSecond = 1'000'000'000ULL;

bool finished(const KnownOrder* k) noexcept {
    return k == nullptr || k->status == OrderStatus::filled || k->status == OrderStatus::cancelled ||
           k->status == OrderStatus::rejected || k->status == OrderStatus::replaced;
}
}  // namespace

// ---- TwoSidedQuoter -----------------------------------------------------------

void TwoSidedQuoter::on_start(Context& c) {
    observe_start(c);
    start_timer_ = c.set_timer(p_.quote_start_ns);
    requote(c);
}

void TwoSidedQuoter::on_market(Context& c, const MarketEvent& m) {
    observe_market(c, m);
    slots_[0].blocked = false;
    slots_[1].blocked = false;
    requote(c);
}

void TwoSidedQuoter::on_order(Context& c, const OrderUpdate& u) {
    for (Slot& sl : slots_) {
        if (sl.id != u.id) continue;
        if (u.kind == UpdateKind::rejected) {
            sl = Slot{};
            sl.blocked = true;
        } else if (u.kind == UpdateKind::cancelled && u.open_qty == 0) {
            sl = Slot{};
        }
    }
    requote(c);
}

void TwoSidedQuoter::on_timer(Context& c, std::uint64_t id) {
    if (id != start_timer_) observe_timer(c, id);
    requote(c);
}

void TwoSidedQuoter::requote(Context& c) {
    const auto bb = c.best_bid();
    const auto ba = c.best_ask();
    const char st = c.trading_state();
    Targets t;
    if (c.now() >= p_.quote_start_ns && bb && ba && (st == 'T' || st == 0)) t = targets(c);
    const std::int64_t pos = c.position();
    if (pos >= p_.max_inventory) t.bid.reset();
    if (pos <= -p_.max_inventory) t.ask.reset();
    // Send the side that is in the way first: if the new bid would reach our
    // own resting offer, that offer must be moved before the bid arrives, or
    // the exchange rejects the bid as a self-cross (actions with the same
    // latency arrive in the order they were sent).
    const Slot& sell = slots_[book::index(Side::sell)];
    const bool sell_first = t.bid && sell.id != 0 && sell.price <= *t.bid;
    if (sell_first) requote_side(c, Side::sell, t.ask, ba ? ba->price : 0);
    requote_side(c, Side::buy, t.bid, bb ? bb->price : 0);
    if (!sell_first) requote_side(c, Side::sell, t.ask, ba ? ba->price : 0);
}

void TwoSidedQuoter::requote_side(Context& c, Side s, std::optional<Price> target, Price seen_best) {
    Slot& sl = slots_[book::index(s)];
    if (sl.id != 0 && finished(c.order(sl.id))) {
        const bool blocked = sl.blocked;
        sl = Slot{};
        sl.blocked = blocked;
    }
    const std::int64_t pos = c.position();
    const std::int64_t room = s == Side::buy ? p_.max_inventory - pos : p_.max_inventory + pos;
    const Qty size = static_cast<Qty>(std::clamp<std::int64_t>(room, 0, static_cast<std::int64_t>(p_.size)));
    if (!target || size == 0) {
        if (sl.id != 0 && !sl.cancelling) {
            c.cancel(sl.id);
            sl.cancelling = true;
        }
        return;
    }
    if (sl.cancelling || sl.blocked) return;
    if (sl.id != 0 && sl.price == *target) return;
    ++stats_.quotes;
    if (seen_best == 0 || *target == seen_best) {
        ++stats_.at_best;
    } else if (book::better(s, *target, seen_best)) {
        ++stats_.improved;
    } else {
        ++stats_.behind;
    }
    if (sl.id == 0) {
        OrderFlags f;
        f.post_only = true;
        sl.id = c.place(s, *target, size, f);
    } else {
        sl.id = c.replace(sl.id, *target, size);
    }
    sl.price = *target;
}

// ---- SymmetricInsideQuoter ----------------------------------------------------

TwoSidedQuoter::Targets SymmetricInsideQuoter::prices(Price b, Price a) noexcept {
    if (b + kCent < a - kCent) return {b + kCent, a - kCent};
    return {b, a};
}

TwoSidedQuoter::Targets SymmetricInsideQuoter::targets(Context& c) {
    return prices(c.best_bid()->price, c.best_ask()->price);
}

// ---- Avellaneda-Stoikov -------------------------------------------------------

AsQuote as_quote(double mid, std::int64_t q, double var, double mean_dist, double tau, double gamma, Price best_bid,
                 Price best_ask) noexcept {
    AsQuote out;
    const double risk = gamma * var * tau;  // $ per share of inventory
    out.reservation = mid - static_cast<double>(q) * risk;
    // delta/2 = gamma sigma^2 (T-t) / 2 + (1/gamma) ln(1 + gamma/k), with 1/k = mean_dist.
    out.half_spread = std::max(0.5 * risk + std::log1p(gamma * mean_dist) / gamma, 0.005);
    // Round away from the mid: bid down to the nearest cent (ties down), ask up (ties up).
    const double xb = (out.reservation - out.half_spread) / 0.01;
    const double xa = (out.reservation + out.half_spread) / 0.01;
    Price bid = static_cast<Price>(std::ceil(xb - 0.5)) * kCent;
    Price ask = static_cast<Price>(std::floor(xa + 0.5)) * kCent;
    // Post-only: never cross the real book.
    bid = std::min(bid, best_ask - kCent);
    ask = std::max(ask, best_bid + kCent);
    if (bid > 0) out.bid = bid;
    if (ask > 0) out.ask = ask;
    return out;
}

std::int64_t AvellanedaStoikov::mid2(const Context& c) noexcept {
    const auto b = c.best_bid();
    const auto a = c.best_ask();
    if (!b || !a) return 0;
    return static_cast<std::int64_t>(b->price) + static_cast<std::int64_t>(a->price);
}

double AvellanedaStoikov::variance_per_second() const noexcept {
    if (count_ == 0) return 0.0;
    // mid change in $ = (change of mid2) / 2 / 1e4, so squared: / 4e8; then
    // per sampling interval -> per second.
    const double per_interval = static_cast<double>(sum_sq_) / static_cast<double>(count_) / 4e8;
    return per_interval * static_cast<double>(kSecond) / static_cast<double>(a_.sample_interval_ns);
}

double AvellanedaStoikov::mean_exec_distance() const noexcept {
    if (n_exec_ == 0) return 0.0;
    return static_cast<double>(sum_dist2_) / static_cast<double>(n_exec_) / 2e4;
}

void AvellanedaStoikov::observe_start(Context& c) {
    sq_.assign(a_.sigma_window, 0);
    last_sample_ = 0;
    prev_mid2_ = mid2(c);
    sample_timer_ = c.set_timer(c.now() + a_.sample_interval_ns);
}

void AvellanedaStoikov::observe_market(Context& c, const MarketEvent& m) {
    if ((m.type == 'E' || (m.type == 'C' && m.flag == 'Y')) && prev_mid2_ != 0) {
        sum_dist2_ += std::llabs(2 * static_cast<std::int64_t>(m.price) - prev_mid2_);
        ++n_exec_;
    }
    prev_mid2_ = mid2(c);
}

void AvellanedaStoikov::observe_timer(Context& c, std::uint64_t id) {
    if (id != sample_timer_) return;
    const std::int64_t m = mid2(c);
    if (m != 0 && last_sample_ != 0 && !sq_.empty()) {
        const std::int64_t d = m - last_sample_;
        if (count_ == sq_.size()) {
            sum_sq_ -= sq_[head_];
        } else {
            ++count_;
        }
        sq_[head_] = d * d;
        sum_sq_ += d * d;
        head_ = (head_ + 1) % sq_.size();
    }
    last_sample_ = m;
    sample_timer_ = c.set_timer(c.now() + a_.sample_interval_ns);
}

TwoSidedQuoter::Targets AvellanedaStoikov::targets(Context& c) {
    if (!ready()) return {};
    const Price b = c.best_bid()->price;
    const Price a = c.best_ask()->price;
    const double mid = static_cast<double>(b + a) / 2.0 / 1e4;
    const double tau = c.now() >= a_.horizon_end_ns
                           ? 0.0
                           : static_cast<double>(a_.horizon_end_ns - c.now()) / static_cast<double>(kSecond);
    const AsQuote q = as_quote(mid, c.position(), variance_per_second(), mean_exec_distance(), tau, a_.gamma, b, a);
    return {q.bid, q.ask};
}

}  // namespace tickforge::sim
