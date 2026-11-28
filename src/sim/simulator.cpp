#include "tickforge/sim/simulator.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include "tickforge/book/itch_replay.hpp"
#include "tickforge/io/byte_source.hpp"
#include "tickforge/itch/compact_file.hpp"
#include "tickforge/itch/stream.hpp"

namespace tickforge::sim {

const char* to_string(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::pending: return "pending";
        case OrderStatus::active: return "active";
        case OrderStatus::filled: return "filled";
        case OrderStatus::cancelled: return "cancelled";
        case OrderStatus::rejected: return "rejected";
        case OrderStatus::replaced: return "replaced";
    }
    return "?";
}

const char* to_string(Reject r) noexcept {
    switch (r) {
        case Reject::none: return "none";
        case Reject::bad_values: return "bad_values";
        case Reject::post_only_would_cross: return "post_only_would_cross";
        case Reject::self_cross: return "self_cross";
        case Reject::unknown_order: return "unknown_order";
        case Reject::too_late: return "too_late";
        case Reject::after_end: return "after_end";
    }
    return "?";
}

const char* to_string(UpdateKind k) noexcept {
    switch (k) {
        case UpdateKind::accepted: return "accepted";
        case UpdateKind::rejected: return "rejected";
        case UpdateKind::cancelled: return "cancelled";
        case UpdateKind::replaced: return "replaced";
        case UpdateKind::cancel_rejected: return "cancel_rejected";
    }
    return "?";
}

namespace {
constexpr std::uint8_t kClsAction = 0, kClsMeasure = 1, kClsDelivery = 2, kClsTimer = 3;
constexpr Qty kMaxShadowQty = 10'000'000;  // keeps price x shares x 100 inside int64
}  // namespace

// ---- Context implementation -------------------------------------------------

class Simulator::Ctx final : public Context {
public:
    explicit Ctx(Simulator& s) : s_(s) {}

    std::uint64_t now() const override { return s_.tau_; }
    const book::FastBook& book() const override { return s_.vbook_; }
    char trading_state() const override { return s_.vstate_; }

    ShadowId place(Side side, Price price, Qty qty, OrderFlags flags) override {
        if (s_.ended_) {
            ++s_.result_.stats.rejected[static_cast<std::size_t>(Reject::after_end)];
            return 0;
        }
        const ShadowId id = new_order(side, price, qty, flags);
        s_.result_.decisions.push_back({s_.tau_, DecisionKind::place, id, 0, side, price, qty, flags});
        send({DecisionKind::place, id, 0, 0});
        return id;
    }

    void cancel(ShadowId id, Qty shares) override {
        if (s_.ended_) {
            ++s_.result_.stats.rejected[static_cast<std::size_t>(Reject::after_end)];
            return;
        }
        ++s_.result_.stats.cancels_sent;
        const KnownOrder* k = order(id);
        s_.result_.decisions.push_back({s_.tau_, DecisionKind::cancel, id, 0, k ? k->side : Side::buy,
                                        k ? k->price : 0, shares, k ? k->flags : OrderFlags{}});
        send({DecisionKind::cancel, id, 0, shares});
    }

    ShadowId replace(ShadowId id, Price price, Qty qty) override {
        if (s_.ended_) {
            ++s_.result_.stats.rejected[static_cast<std::size_t>(Reject::after_end)];
            return 0;
        }
        const KnownOrder* k = order(id);
        const Side side = k ? k->side : Side::buy;
        const OrderFlags flags = k ? k->flags : OrderFlags{};
        const ShadowId nid = new_order(side, price, qty, flags);
        s_.result_.decisions.push_back({s_.tau_, DecisionKind::replace, id, nid, side, price, qty, flags});
        send({DecisionKind::replace, id, nid, 0});
        return nid;
    }

    std::uint64_t set_timer(std::uint64_t at_ns) override {
        if (s_.ended_) return 0;
        const std::uint64_t id = ++s_.timer_seq_;
        s_.schedule(std::max(at_ns, s_.tau_), kClsTimer, EvKind::timer, id);
        return id;
    }

    std::int64_t position() const override { return s_.known_position_; }
    Micros cash() const override { return s_.known_cash_; }
    const KnownOrder* order(ShadowId id) const override {
        if (id == 0 || id > s_.known_.size()) return nullptr;
        return &s_.known_[id - 1];
    }

private:
    ShadowId new_order(Side side, Price price, Qty qty, OrderFlags flags) {
        ++s_.result_.stats.orders_sent;
        const ShadowId id = s_.shadows_.size() + 1;
        Shadow sh;
        sh.id = id;
        sh.side = side;
        sh.price = price;
        sh.qty = qty;
        sh.open = qty;
        sh.flags = flags;
        s_.shadows_.push_back(sh);
        KnownOrder k;
        k.id = id;
        k.side = side;
        k.price = price;
        k.qty = qty;
        k.open = qty;
        k.flags = flags;
        s_.known_.push_back(k);
        return id;
    }
    void send(const Action& a) {
        s_.actions_.push_back(a);
        s_.schedule(s_.tau_ + s_.cfg_.order_latency_ns, kClsAction, EvKind::action, s_.actions_.size() - 1);
    }

    Simulator& s_;
};

// ---- Construction and the event loop ----------------------------------------

Simulator::Simulator(SimConfig cfg, Strategy& strategy)
    : cfg_(std::move(cfg)), strategy_(strategy), ctx_(std::make_unique<Ctx>(*this)) {
    if (cfg_.markout_horizons_ns.size() > kMaxMarkoutHorizons)
        throw std::invalid_argument("SimConfig: more than " + std::to_string(kMaxMarkoutHorizons) + " markout horizons");
    schedule(cfg_.start_ns, kClsTimer, EvKind::start);
    if (cfg_.end_ns != kNever) schedule(cfg_.end_ns, kClsMeasure, EvKind::end);
    if (cfg_.pnl_sample_interval_ns > 0) schedule(cfg_.start_ns, kClsMeasure, EvKind::sample);
}

Simulator::~Simulator() = default;

void Simulator::schedule(std::uint64_t time, std::uint8_t cls, EvKind kind, std::uint64_t a, std::uint32_t b) {
    events_.push(Event{time, cls, seq_++, kind, a, b});
}

void Simulator::advance(std::uint64_t t) {
    ++result_.stats.messages;
    run_events(t, false);
    last_ts_ = t;
}

void Simulator::run_events(std::uint64_t limit, bool inclusive) {
    for (;;) {
        const bool have_ev = !events_.empty();
        const bool have_dl = !deliveries_.empty();
        if (!have_ev && !have_dl) return;
        // Pick the earlier of the next queued event and the next delivery,
        // by (time, class); deliveries are class 2 and are already in order.
        bool use_dl;
        if (!have_ev) {
            use_dl = true;
        } else if (!have_dl) {
            use_dl = false;
        } else {
            const Event& e = events_.top();
            const Delivery& d = deliveries_.front();
            use_dl = d.time != e.time ? d.time < e.time : kClsDelivery < e.cls;
        }
        const std::uint64_t t = use_dl ? deliveries_.front().time : events_.top().time;
        if (t > limit || (!inclusive && t == limit)) return;
        if (use_dl) {
            const Delivery d = deliveries_.front();
            deliveries_.pop_front();
            process_delivery(d);
        } else {
            const Event e = events_.top();
            events_.pop();
            process(e);
        }
    }
}

template <typename F>
void Simulator::call_strategy(std::uint64_t tau, F&& f) {
    tau_ = tau;
    ++result_.stats.strategy_callbacks;
    // No-look-ahead check: everything the strategy's book holds must be
    // timestamped at or before tau - market-data latency.
    if (seen_any_ && latest_seen_ts_ + cfg_.market_data_latency_ns > tau) ++result_.stats.lookahead_violations;
    f();
}

void Simulator::process(const Event& e) {
    switch (e.kind) {
        case EvKind::action: {
            const Action a = actions_[e.a];
            if (ended_) {
                ++result_.stats.rejected[static_cast<std::size_t>(Reject::after_end)];
                return;
            }
            switch (a.kind) {
                case DecisionKind::place: arrive_new(shadows_[a.id - 1], e.time); break;
                case DecisionKind::cancel: arrive_cancel(a.id, a.shares, e.time); break;
                case DecisionKind::replace: arrive_replace(a.id, a.new_id, e.time); break;
            }
            return;
        }
        case EvKind::markout: {
            FillRecord& r = result_.fills[e.a];
            const std::int64_t m = mid2_now();
            if (m != 0 && r.mid2_before != 0) {
                r.mid2_after[e.b] = m;
                r.markout_ok[e.b] = true;
            } else {
                ++result_.stats.markouts_unavailable[e.b];
            }
            return;
        }
        case EvKind::sample:
            if (ended_) return;
            sample(e.time);
            if (cfg_.end_ns == kNever || e.time + cfg_.pnl_sample_interval_ns <= cfg_.end_ns)
                schedule(e.time + cfg_.pnl_sample_interval_ns, kClsMeasure, EvKind::sample);
            return;
        case EvKind::end:
            if (!ended_) end_run(e.time);
            return;
        case EvKind::timer:
            if (started_ && !ended_) call_strategy(e.time, [&] { strategy_.on_timer(*ctx_, e.a); });
            return;
        case EvKind::start:
            if (ended_) return;
            started_ = true;
            call_strategy(e.time, [&] { strategy_.on_start(*ctx_); });
            return;
    }
}

void Simulator::process_delivery(const Delivery& d) {
    switch (d.kind) {
        case DeliveryKind::market: {
            const MarketEvent& m = d.market;
            switch (m.type) {
                case 'A':
                case 'F': vbook_.add(m.ref, m.side, m.price, m.shares); break;
                case 'E':
                case 'C': vbook_.execute(m.ref, m.shares); break;
                case 'X': vbook_.cancel(m.ref, m.shares); break;
                case 'D': vbook_.remove(m.ref); break;
                case 'U': vbook_.replace(m.ref, m.new_ref, m.price, m.shares); break;
                case 'H': vstate_ = m.flag; break;
                default: break;
            }
            latest_seen_ts_ = m.ts;
            seen_any_ = true;
            ++result_.stats.stock_messages;
            if (started_ && !ended_) call_strategy(d.time, [&] { strategy_.on_market(*ctx_, m); });
            return;
        }
        case DeliveryKind::update: {
            const OrderUpdate& u = d.update;
            KnownOrder& k = known_[u.id - 1];
            switch (u.kind) {
                case UpdateKind::accepted:
                    k.status = OrderStatus::active;
                    k.open = u.open_qty;
                    break;
                case UpdateKind::rejected:
                    k.status = OrderStatus::rejected;
                    k.open = 0;
                    break;
                case UpdateKind::cancelled:
                    k.open = u.open_qty;
                    if (u.open_qty == 0) k.status = OrderStatus::cancelled;
                    break;
                case UpdateKind::replaced:
                    k.status = OrderStatus::replaced;
                    k.open = 0;
                    break;
                case UpdateKind::cancel_rejected: break;
            }
            if (started_ && !ended_) call_strategy(d.time, [&] { strategy_.on_order(*ctx_, u); });
            return;
        }
        case DeliveryKind::fill: {
            const FillReport& f = d.fill;
            KnownOrder& k = known_[f.id - 1];
            k.open = f.open_qty;
            k.filled += f.qty;
            if (f.open_qty == 0) k.status = OrderStatus::filled;
            const Micros notional = static_cast<Micros>(f.price) * f.qty * kMicrosPerTick;
            known_cash_ += (f.side == Side::buy ? -notional : notional) - f.fee;
            known_position_ += f.side == Side::buy ? static_cast<std::int64_t>(f.qty) : -static_cast<std::int64_t>(f.qty);
            if (started_ && !ended_) call_strategy(d.time, [&] { strategy_.on_fill(*ctx_, f); });
            return;
        }
    }
}

void Simulator::deliver_market(const MarketEvent& m) {
    Delivery d{};
    d.time = m.ts + cfg_.market_data_latency_ns;
    d.kind = DeliveryKind::market;
    d.market = m;
    deliveries_.push_back(d);
}

void Simulator::deliver_update(const OrderUpdate& u) {
    Delivery d{};
    d.time = u.exchange_ts + cfg_.market_data_latency_ns;
    d.kind = DeliveryKind::update;
    d.update = u;
    deliveries_.push_back(d);
}

void Simulator::deliver_fill(const FillReport& f) {
    Delivery d{};
    d.time = f.exchange_ts + cfg_.market_data_latency_ns;
    d.kind = DeliveryKind::fill;
    d.fill = f;
    deliveries_.push_back(d);
}

// ---- Exchange side: shadow actions ---------------------------------------------

std::int64_t Simulator::mid2_now() noexcept {
    const auto b = xbook_.best(Side::buy);
    const auto a = xbook_.best(Side::sell);
    if (!b || !a) return 0;
    last_mid2_ = b->price + a->price;
    return last_mid2_;
}

Simulator::Shadow* Simulator::shadow(ShadowId id) noexcept {
    if (id == 0 || id > shadows_.size()) return nullptr;
    return &shadows_[id - 1];
}

void Simulator::reject(Shadow& s, Reject why, std::uint64_t t) {
    s.status = OrderStatus::rejected;
    s.open = 0;
    ++result_.stats.rejected[static_cast<std::size_t>(why)];
    OrderUpdate u;
    u.exchange_ts = t;
    u.id = s.id;
    u.kind = UpdateKind::rejected;
    u.reason = why;
    deliver_update(u);
}

void Simulator::arrive_new(Shadow& s, std::uint64_t t) {
    if (s.qty == 0 || s.qty > kMaxShadowQty || s.price <= 0) return reject(s, Reject::bad_values, t);
    for (ShadowId id : active_) {
        const Shadow& o = shadows_[id - 1];
        if (o.side != s.side && book::crosses(s.side, s.price, o.price)) return reject(s, Reject::self_cross, t);
    }
    const auto opp = xbook_.best(opposite(s.side));
    const bool would_cross = opp && book::crosses(s.side, s.price, opp->price);
    if (s.flags.post_only && would_cross) return reject(s, Reject::post_only_would_cross, t);

    // Accepted: its place in the queue is fixed now (Rule G).
    s.status = OrderStatus::active;
    s.key = max_ref_;
    s.seq = ++arrival_seq_;
    s.ahead = 0;
    xbook_.for_each_order_at(s.side, s.price, [&](OrderId, Qty q) { s.ahead += q; });
    ++result_.stats.accepted;
    result_.stats.placed_shares += s.qty;
    OrderUpdate u;
    u.exchange_ts = t;
    u.id = s.id;
    u.kind = UpdateKind::accepted;
    u.open_qty = s.open;
    deliver_update(u);

    if (would_cross && trading()) take(s, t);
    if (s.status != OrderStatus::active) return;  // filled completely by taking
    if (s.flags.ioc) {
        OrderUpdate c;
        c.exchange_ts = t;
        c.id = s.id;
        c.kind = UpdateKind::cancelled;
        c.cancelled_qty = s.open;
        c.open_qty = 0;
        s.open = 0;
        s.status = OrderStatus::cancelled;
        deliver_update(c);
        return;
    }
    active_.push_back(s.id);
}

void Simulator::take(Shadow& s, std::uint64_t t) {
    const std::int64_t mid2 = mid2_now();
    const Side opp = opposite(s.side);
    std::vector<book::LevelView> levels(xbook_.level_count(opp));
    levels.resize(xbook_.depth(opp, levels.data(), levels.size()));
    std::vector<std::pair<OrderId, Qty>> orders;
    for (const book::LevelView& lv : levels) {
        if (s.open == 0 || !book::crosses(s.side, s.price, lv.price)) break;
        orders.clear();
        xbook_.for_each_order_at(opp, lv.price, [&](OrderId id, Qty q) { orders.emplace_back(id, q); });
        for (const auto& [id, q] : orders) {
            if (s.open == 0) break;
            Qty& used = taken_[id];
            const Qty avail = q > used ? q - used : 0;
            const Qty n = std::min(avail, s.open);
            if (n == 0) continue;
            used += n;
            fill(s, n, lv.price, Liquidity::taker, 'T', t, mid2);
        }
    }
}

void Simulator::arrive_cancel(ShadowId id, Qty shares, std::uint64_t t) {
    Shadow* s = shadow(id);
    OrderUpdate u;
    u.exchange_ts = t;
    u.id = id;
    if (s == nullptr || s->status == OrderStatus::pending || s->status == OrderStatus::rejected) {
        u.kind = UpdateKind::cancel_rejected;
        u.reason = Reject::unknown_order;
        ++result_.stats.rejected[static_cast<std::size_t>(Reject::unknown_order)];
        if (s == nullptr) return;  // no such order: nothing to report it to
        deliver_update(u);
        return;
    }
    if (s->status != OrderStatus::active) {
        u.kind = UpdateKind::cancel_rejected;
        u.reason = Reject::too_late;
        ++result_.stats.rejected[static_cast<std::size_t>(Reject::too_late)];
        deliver_update(u);
        return;
    }
    u.kind = UpdateKind::cancelled;
    if (shares == 0 || shares >= s->open) {
        u.cancelled_qty = s->open;
        s->open = 0;
        s->status = OrderStatus::cancelled;
        remove_active(id);
    } else {
        u.cancelled_qty = shares;
        s->open -= shares;  // keeps its key: priority unchanged
    }
    u.open_qty = s->open;
    deliver_update(u);
}

void Simulator::arrive_replace(ShadowId old_id, ShadowId new_id, std::uint64_t t) {
    Shadow* o = shadow(old_id);
    Shadow& n = shadows_[new_id - 1];
    if (o == nullptr || o->status != OrderStatus::active) {
        const bool unknown = o == nullptr || o->status == OrderStatus::pending || o->status == OrderStatus::rejected;
        return reject(n, unknown ? Reject::unknown_order : Reject::too_late, t);
    }
    o->status = OrderStatus::replaced;
    o->open = 0;
    remove_active(old_id);
    OrderUpdate u;
    u.exchange_ts = t;
    u.id = old_id;
    u.kind = UpdateKind::replaced;
    u.new_id = new_id;
    deliver_update(u);
    arrive_new(n, t);
}

void Simulator::remove_active(ShadowId id) {
    const auto it = std::find(active_.begin(), active_.end(), id);
    if (it != active_.end()) active_.erase(it);
}

void Simulator::fill(Shadow& s, Qty q, Price px, Liquidity liq, char trigger, std::uint64_t t,
                     std::int64_t mid2_before) {
    s.open -= q;
    s.filled += q;
    const Micros notional = static_cast<Micros>(px) * q * kMicrosPerTick;
    const Micros fee = liq == Liquidity::maker ? -cfg_.fees.maker_rebate_per_share * static_cast<Micros>(q)
                                               : cfg_.fees.taker_fee_per_share * static_cast<Micros>(q);
    cash_ += (s.side == Side::buy ? -notional : notional) - fee;
    inventory_ += s.side == Side::buy ? static_cast<std::int64_t>(q) : -static_cast<std::int64_t>(q);

    SimStats& st = result_.stats;
    ++st.fills;
    st.filled_shares += q;
    (liq == Liquidity::maker ? st.maker_shares : st.taker_shares) += q;
    (s.side == Side::buy ? st.bought_shares : st.sold_shares) += q;
    if (fee < 0) st.rebates += -fee; else st.fees_paid += fee;
    st.max_inventory = std::max(st.max_inventory, inventory_);
    st.min_inventory = std::min(st.min_inventory, inventory_);

    FillRecord r;
    r.report = FillReport{t, s.id, s.side, px, q, liq, fee, s.open};
    r.trigger = trigger;
    r.mid2_before = mid2_before;
    result_.fills.push_back(r);
    const std::uint64_t idx = result_.fills.size() - 1;
    for (std::uint32_t h = 0; h < cfg_.markout_horizons_ns.size(); ++h)
        schedule(t + cfg_.markout_horizons_ns[h], kClsMeasure, EvKind::markout, idx, h);
    sample(t);
    deliver_fill(r.report);
    if (s.open == 0) {
        s.status = OrderStatus::filled;
        remove_active(s.id);
    }
}

void Simulator::sample(std::uint64_t t) {
    const std::int64_t m = mid2_now();
    const std::int64_t mark = m != 0 ? m : last_mid2_;
    result_.pnl.push_back({t, cash_, inventory_, m, cash_ + inventory_ * mark * (kMicrosPerTick / 2)});
}

void Simulator::end_run(std::uint64_t t) {
    ended_ = true;
    for (ShadowId id : active_) {
        Shadow& s = shadows_[id - 1];
        s.status = OrderStatus::cancelled;
        s.open = 0;
    }
    active_.clear();
    sample(t);
    result_.end_ts = t;
    result_.final_cash = cash_;
    result_.final_inventory = inventory_;
    result_.final_mid2 = result_.pnl.back().mid2 != 0 ? result_.pnl.back().mid2 : last_mid2_;
    result_.final_mtm = result_.pnl.back().mtm;
    call_strategy(t, [&] { strategy_.on_end(*ctx_); });
}

// ---- Exchange side: real messages --------------------------------------------------

// Active shadows on `side` that a trade at `trade_price` (or a real order
// priced at it, arriving on the other side) reaches, best priority first.
std::vector<ShadowId> Simulator::candidates(Side side, Price trade_price) const {
    std::vector<ShadowId> out;
    for (ShadowId id : active_) {
        const Shadow& s = shadows_[id - 1];
        if (s.side == side && !book::better(side, trade_price, s.price)) out.push_back(id);
    }
    std::sort(out.begin(), out.end(), [&](ShadowId a, ShadowId b) {
        const Shadow& x = shadows_[a - 1];
        const Shadow& y = shadows_[b - 1];
        if (x.price != y.price) return book::better(side, x.price, y.price);
        return x.seq < y.seq;
    });
    return out;
}

void Simulator::late_add(Side side, Price price, OrderId ref, Qty shares) {
    for (ShadowId id : active_) {
        Shadow& s = shadows_[id - 1];
        if (s.side == side && s.price == price && ref <= s.key) s.ahead += shares;
    }
}

void Simulator::reduce_ahead(Side side, Price price, OrderId ref, std::uint64_t shares) {
    for (ShadowId id : active_) {
        Shadow& s = shadows_[id - 1];
        if (s.side != side || s.price != price || ref > s.key) continue;
        if (shares > s.ahead) {
            ++result_.stats.ahead_underflow;
            s.ahead = 0;
        } else {
            s.ahead -= shares;
        }
    }
}

void Simulator::after_real_reduce(OrderId ref) {
    const auto it = taken_.find(ref);
    if (it == taken_.end()) return;
    const auto o = xbook_.find(ref);
    if (!o) {
        taken_.erase(it);
    } else {
        it->second = std::min(it->second, o->qty);
    }
}

void Simulator::crossing_add(Side real_side, Price price, Qty shares, char type, std::uint64_t t, std::int64_t mid2) {
    if (!trading() || active_.empty()) return;
    const Side side = opposite(real_side);  // the shadows it would have traded with
    const std::vector<ShadowId> c = candidates(side, price);
    Qty left = shares;
    for (ShadowId id : c) {
        Shadow& s = shadows_[id - 1];
        if (cfg_.model == FillModel::naive_touch) {
            fill(s, s.open, s.price, Liquidity::maker, type, t, mid2);
            continue;
        }
        if (left == 0) break;
        if (s.ahead > 0) {
            ++result_.stats.anomaly_cross_add_with_ahead;
            continue;
        }
        const Qty n = std::min(left, s.open);
        left -= n;
        fill(s, n, s.price, Liquidity::maker, type, t, mid2);
    }
}

// Naive model: any print at or through a shadow's price fills it completely.
void Simulator::touch(Price trade_price, char type, std::uint64_t t, std::int64_t mid2) {
    if (!trading() || active_.empty()) return;
    for (Side side : {Side::buy, Side::sell}) {
        for (ShadowId id : candidates(side, trade_price)) {
            Shadow& s = shadows_[id - 1];
            fill(s, s.open, s.price, Liquidity::maker, type, t, mid2);
        }
    }
}

void Simulator::real_add(const itch::Header& h, OrderId ref, Side side, Price price, Qty shares, char type) {
    const std::int64_t mid2 = mid2_now();
    crossing_add(side, price, shares, type, h.timestamp_ns, mid2);
    late_add(side, price, ref, shares);
    xbook_.add(ref, side, price, shares);
    max_ref_ = std::max(max_ref_, ref);
    deliver_market({h.timestamp_ns, type, side, price, shares, ref, 0, 0});
}

void Simulator::real_exec(const itch::Header& h, OrderId ref, Qty shares, bool priced, Price exec_price, char type,
                          char printable) {
    const auto o = xbook_.find(ref);
    if (!o) return;
    const Price e = priced ? exec_price : o->price;
    const std::uint64_t t = h.timestamp_ns;
    if (trading() && !active_.empty()) {
        const std::int64_t mid2 = mid2_now();
        if (cfg_.model == FillModel::naive_touch) {
            touch(e, type, t, mid2);
        } else {
            // Rule H.1: the aggressor would have reached every shadow on this
            // side priced at e or better that is not behind R.
            Qty left = shares;
            for (ShadowId id : candidates(o->side, e)) {
                if (left == 0) break;
                Shadow& s = shadows_[id - 1];
                const bool r_ahead = o->price == s.price && ref <= s.key;
                if (r_ahead) continue;
                if (s.ahead > 0) {
                    ++result_.stats.anomaly_exec_with_ahead;
                    continue;
                }
                const Qty n = std::min(left, s.open);
                left -= n;
                fill(s, n, s.price, Liquidity::maker, type, t, mid2);
            }
        }
    }
    reduce_ahead(o->side, o->price, ref, shares);
    xbook_.execute(ref, shares);
    after_real_reduce(ref);
    deliver_market({t, type, o->side, e, shares, ref, 0, printable});
}

void Simulator::on(const itch::StockDirectory& m) {
    advance(m.header.timestamp_ns);
    if (!symbol_found_ && m.stock.view() == cfg_.symbol) {
        symbol_found_ = true;
        locate_ = m.header.stock_locate;
        result_.symbol_found = true;
        result_.locate = locate_;
    }
}

void Simulator::on(const itch::StockTradingAction& m) {
    advance(m.header.timestamp_ns);
    if (!target(m.header)) return;
    xstate_ = m.trading_state;
    MarketEvent ev{};
    ev.ts = m.header.timestamp_ns;
    ev.type = 'H';
    ev.flag = m.trading_state;
    deliver_market(ev);
}

void Simulator::on(const itch::AddOrder& m) {
    advance(m.header.timestamp_ns);
    if (target(m.header))
        real_add(m.header, m.order_reference_number, book::side_from_itch(m.side), m.price, m.shares, 'A');
}

void Simulator::on(const itch::AddOrderMpid& m) {
    advance(m.header.timestamp_ns);
    if (target(m.header))
        real_add(m.header, m.order_reference_number, book::side_from_itch(m.side), m.price, m.shares, 'F');
}

void Simulator::on(const itch::OrderExecuted& m) {
    advance(m.header.timestamp_ns);
    if (target(m.header)) real_exec(m.header, m.order_reference_number, m.executed_shares, false, 0, 'E', 0);
}

void Simulator::on(const itch::OrderExecutedWithPrice& m) {
    advance(m.header.timestamp_ns);
    if (target(m.header))
        real_exec(m.header, m.order_reference_number, m.executed_shares, true, m.execution_price, 'C', m.printable);
}

void Simulator::on(const itch::OrderCancel& m) {
    advance(m.header.timestamp_ns);
    if (!target(m.header)) return;
    const auto o = xbook_.find(m.order_reference_number);
    if (!o) return;
    reduce_ahead(o->side, o->price, m.order_reference_number, m.cancelled_shares);
    xbook_.cancel(m.order_reference_number, m.cancelled_shares);
    after_real_reduce(m.order_reference_number);
    deliver_market({m.header.timestamp_ns, 'X', o->side, o->price, m.cancelled_shares, m.order_reference_number, 0, 0});
}

void Simulator::on(const itch::OrderDelete& m) {
    advance(m.header.timestamp_ns);
    if (!target(m.header)) return;
    const auto o = xbook_.find(m.order_reference_number);
    if (!o) return;
    reduce_ahead(o->side, o->price, m.order_reference_number, o->qty);
    xbook_.remove(m.order_reference_number);
    taken_.erase(m.order_reference_number);
    deliver_market({m.header.timestamp_ns, 'D', o->side, o->price, o->qty, m.order_reference_number, 0, 0});
}

void Simulator::on(const itch::OrderReplace& m) {
    advance(m.header.timestamp_ns);
    if (!target(m.header)) return;
    const OrderId old_ref = m.original_order_reference_number;
    const OrderId new_ref = m.new_order_reference_number;
    const auto o = xbook_.find(old_ref);
    if (!o) return;
    const std::int64_t mid2 = mid2_now();
    reduce_ahead(o->side, o->price, old_ref, o->qty);
    crossing_add(o->side, m.price, m.shares, 'U', m.header.timestamp_ns, mid2);
    late_add(o->side, m.price, new_ref, m.shares);
    xbook_.replace(old_ref, new_ref, m.price, m.shares);
    taken_.erase(old_ref);
    max_ref_ = std::max(max_ref_, new_ref);
    deliver_market({m.header.timestamp_ns, 'U', o->side, static_cast<Price>(m.price), m.shares, old_ref, new_ref, 0});
}

void Simulator::on(const itch::Trade& m) {
    advance(m.header.timestamp_ns);
    if (!target(m.header)) return;
    if (cfg_.model == FillModel::naive_touch) touch(m.price, 'P', m.header.timestamp_ns, mid2_now());
    deliver_market({m.header.timestamp_ns, 'P', book::side_from_itch(m.side), m.price, m.shares, 0, 0, 0});
}

void Simulator::on(const itch::CrossTrade& m) {
    advance(m.header.timestamp_ns);
    if (!target(m.header)) return;
    if (cfg_.model == FillModel::naive_touch) touch(m.cross_price, 'Q', m.header.timestamp_ns, mid2_now());
    const Qty shares = m.shares > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<Qty>(m.shares);
    deliver_market({m.header.timestamp_ns, 'Q', Side::buy, m.cross_price, shares, 0, 0, m.cross_type});
}

void Simulator::finish() {
    run_events(last_ts_, true);
    if (!ended_) end_run(last_ts_);
    // Whatever is still queued lies after the last message: markouts there
    // cannot be measured; deliveries, timers and actions never happen.
    while (!events_.empty()) {
        const Event e = events_.top();
        events_.pop();
        if (e.kind == EvKind::markout) ++result_.stats.markouts_unavailable[e.b];
    }
    deliveries_.clear();
}

std::optional<std::uint64_t> Simulator::ahead_of(ShadowId id) const {
    if (id == 0 || id > shadows_.size()) return std::nullopt;
    const Shadow& s = shadows_[id - 1];
    if (s.status != OrderStatus::active) return std::nullopt;
    return s.ahead;
}

OrderStatus Simulator::exchange_status(ShadowId id) const {
    if (id == 0 || id > shadows_.size()) return OrderStatus::rejected;
    return shadows_[id - 1].status;
}

Qty Simulator::exchange_open(ShadowId id) const {
    if (id == 0 || id > shadows_.size()) return 0;
    return shadows_[id - 1].open;
}

SimResult run_file(const std::string& path, const SimConfig& cfg, Strategy& strategy) {
    std::unique_ptr<io::ByteSource> src;
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".tfc") == 0) {
        src = std::make_unique<itch::CompactReader>(path);
    } else {
        src = io::open_byte_source(path);
    }
    auto sim = std::make_unique<Simulator>(cfg, strategy);
    itch::parse_stream(*src, *sim);
    sim->finish();
    return sim->result();
}

}  // namespace tickforge::sim
