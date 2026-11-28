#include "tickforge/sim/strategies.hpp"

namespace tickforge::sim {

namespace {
bool finished(const KnownOrder* k) noexcept {
    return k == nullptr || k->status == OrderStatus::filled || k->status == OrderStatus::cancelled ||
           k->status == OrderStatus::rejected || k->status == OrderStatus::replaced;
}
}  // namespace

void JoinBestQuoter::on_market(Context& c, const MarketEvent&) {
    slots_[0].blocked = false;
    slots_[1].blocked = false;
    requote(c);
}

void JoinBestQuoter::on_order(Context& c, const OrderUpdate& u) {
    for (Slot& sl : slots_) {
        if (sl.id != u.id) continue;
        if (u.kind == UpdateKind::rejected) {
            sl = Slot{};
            sl.blocked = true;
        } else if (u.kind == UpdateKind::cancelled && u.open_qty == 0) {
            sl = Slot{};
            requote(c);
        }
    }
}

void JoinBestQuoter::requote(Context& c) {
    requote_side(c, Side::buy);
    requote_side(c, Side::sell);
}

void JoinBestQuoter::requote_side(Context& c, Side s) {
    Slot& sl = slots_[book::index(s)];
    if (sl.id != 0 && finished(c.order(sl.id))) {
        const bool blocked = sl.blocked;
        sl = Slot{};
        sl.blocked = blocked;
    }
    const auto best = s == Side::buy ? c.best_bid() : c.best_ask();
    const bool allowed = s == Side::buy ? c.position() < max_inv_ : c.position() > -max_inv_;
    if (!best || !allowed) {
        if (sl.id != 0 && !sl.cancelling) {
            c.cancel(sl.id);
            sl.cancelling = true;
        }
        return;
    }
    if (sl.cancelling || sl.blocked) return;
    if (sl.id == 0) {
        OrderFlags f;
        f.post_only = true;
        sl.id = c.place(s, best->price, size_, f);
        sl.price = best->price;
    } else if (sl.price != best->price) {
        sl.id = c.replace(sl.id, best->price, size_);
        sl.price = best->price;
    }
}

void ScheduledOrders::on_start(Context& c) {
    for (std::size_t i = 0; i < plan_.size(); ++i) timer_to_step_[c.set_timer(plan_[i].at_ns)] = i;
}

void ScheduledOrders::on_timer(Context& c, std::uint64_t timer_id) {
    const auto it = timer_to_step_.find(timer_id);
    if (it == timer_to_step_.end()) return;
    const std::size_t i = it->second;
    const ScheduledAction& a = plan_[i];
    switch (a.kind) {
        case DecisionKind::place: ids_[i] = c.place(a.side, a.price, a.qty, a.flags); break;
        case DecisionKind::cancel: c.cancel(ids_[a.target], a.qty); break;
        case DecisionKind::replace: ids_[i] = c.replace(ids_[a.target], a.price, a.qty); break;
    }
}

}  // namespace tickforge::sim
