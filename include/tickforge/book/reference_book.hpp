#pragma once
// ReferenceBook: a deliberately simple order book, written to be obviously
// correct rather than fast. It is the "answer key" the FastBook is checked
// against in the differential fuzz test and on the real day.
//
//   bids: std::map<price, queue>, highest price first
//   asks: std::map<price, queue>, lowest price first
//   queue: std::list of {id, shares}; front = oldest = trades first
//   index: std::unordered_map<id, where the order lives>
//
// Every operation follows the same rules, and checks errors in the same
// order, as FastBook (see fast_book.hpp): id, then shares, then price; an
// error leaves the book unchanged.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "tickforge/book/types.hpp"

namespace tickforge::book {

class ReferenceBook {
public:
    void reserve(std::size_t orders, std::size_t /*levels_per_side*/) { index_.reserve(orders); }

    // ---- Replay mode ---------------------------------------------------------

    Status add(OrderId id, Side side, Price price, Qty qty) {
        if (index_.count(id) != 0) return Status::duplicate_order;
        if (qty == 0) return Status::zero_quantity;
        if (price <= 0) return Status::bad_price;
        rest(id, side, price, qty);
        return Status::ok;
    }

    ExecResult execute(OrderId id, Qty qty) { return reduce(id, qty); }

    Status cancel(OrderId id, Qty qty) { return reduce(id, qty).status; }

    Status remove(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return Status::unknown_order;
        erase_order(it->second);
        return Status::ok;
    }

    Status replace(OrderId old_id, OrderId new_id, Price price, Qty qty) {
        auto it = index_.find(old_id);
        if (it == index_.end()) return Status::unknown_order;
        if (new_id == old_id || index_.count(new_id) != 0) return Status::duplicate_order;
        if (qty == 0) return Status::zero_quantity;
        if (price <= 0) return Status::bad_price;
        const Side side = it->second.side;
        erase_order(it->second);
        rest(new_id, side, price, qty);
        return Status::ok;
    }

    // ---- Matching mode -------------------------------------------------------

    template <FillSink Sink>
    SubmitResult submit_limit(OrderId id, Side side, Price price, Qty qty, Sink&& on_fill) {
        if (index_.count(id) != 0) return {Status::duplicate_order, 0, 0};
        if (qty == 0) return {Status::zero_quantity, 0, 0};
        if (price <= 0) return {Status::bad_price, 0, 0};
        Qty left = qty;
        if (side == Side::buy) {
            left = match(asks_, id, left, [&](Price ask) { return ask <= price; }, on_fill);
        } else {
            left = match(bids_, id, left, [&](Price bid) { return bid >= price; }, on_fill);
        }
        if (left > 0) rest(id, side, price, left);
        return {Status::ok, qty - left, left};
    }

    template <FillSink Sink>
    SubmitResult submit_market(OrderId id, Side side, Qty qty, Sink&& on_fill) {
        if (index_.count(id) != 0) return {Status::duplicate_order, 0, 0};
        if (qty == 0) return {Status::zero_quantity, 0, 0};
        const auto any_price = [](Price) { return true; };
        const Qty left = side == Side::buy ? match(asks_, id, qty, any_price, on_fill)
                                           : match(bids_, id, qty, any_price, on_fill);
        return {Status::ok, qty - left, 0};
    }

    // ---- Queries ---------------------------------------------------------------

    std::optional<LevelView> best(Side side) const {
        if (side == Side::buy) return bids_.empty() ? std::nullopt : std::optional(view(*bids_.begin()));
        return asks_.empty() ? std::nullopt : std::optional(view(*asks_.begin()));
    }

    std::size_t depth(Side side, LevelView* out, std::size_t n) const {
        std::size_t k = 0;
        for_each_level(side, [&](const LevelView& l) {
            if (k < n) out[k++] = l;
        });
        return k;
    }

    std::optional<OrderView> find(OrderId id) const {
        auto it = index_.find(id);
        if (it == index_.end()) return std::nullopt;
        const Where& w = it->second;
        const Queue& q = queue_at(w.side, w.price);
        return OrderView{id, w.side, w.price, w.it->qty, w.it == q.begin()};
    }

    std::size_t order_count() const { return index_.size(); }
    std::size_t level_count(Side side) const { return side == Side::buy ? bids_.size() : asks_.size(); }

    template <typename F>
    void for_each_level(Side side, F&& f) const {
        if (side == Side::buy) {
            for (const auto& level : bids_) f(view(level));
        } else {
            for (const auto& level : asks_) f(view(level));
        }
    }

    template <typename F>
    void for_each_order(Side side, F&& f) const {
        auto visit = [&](const auto& levels) {
            for (const auto& [price, queue] : levels)
                for (const RestingOrder& o : queue) f(price, o.id, o.qty);
        };
        if (side == Side::buy) {
            visit(bids_);
        } else {
            visit(asks_);
        }
    }

private:
    struct RestingOrder {
        OrderId id;
        Qty qty;
    };
    using Queue = std::list<RestingOrder>;
    struct Where {
        Side side;
        Price price;
        Queue::iterator it;
    };

    template <typename Level>
    static LevelView view(const Level& level) {
        LevelView v{level.first, 0, 0};
        for (const RestingOrder& o : level.second) {
            v.shares += o.qty;
            ++v.orders;
        }
        return v;
    }

    const Queue& queue_at(Side side, Price price) const {
        return side == Side::buy ? bids_.at(price) : asks_.at(price);
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        Queue& q = side == Side::buy ? bids_[price] : asks_[price];
        q.push_back({id, qty});
        index_[id] = Where{side, price, std::prev(q.end())};
    }

    void erase_order(const Where& w) {
        const OrderId id = w.it->id;
        if (w.side == Side::buy) {
            erase_from(bids_, w);
        } else {
            erase_from(asks_, w);
        }
        index_.erase(id);  // last: `w` lives inside index_
    }

    template <typename Levels>
    static void erase_from(Levels& levels, const Where& w) {
        auto level = levels.find(w.price);
        level->second.erase(w.it);
        if (level->second.empty()) levels.erase(level);
    }

    ExecResult reduce(OrderId id, Qty qty) {
        auto it = index_.find(id);
        if (it == index_.end()) return {Status::unknown_order, Side::buy, 0, 0};
        Where& w = it->second;
        ExecResult r{Status::ok, w.side, w.price, w.it->qty};
        if (qty == 0) {
            r.status = Status::zero_quantity;
            return r;
        }
        if (qty > w.it->qty) {
            r.status = Status::too_many_shares;
            return r;
        }
        w.it->qty -= qty;
        r.remaining = w.it->qty;
        if (r.remaining == 0) erase_order(w);
        return r;
    }

    template <typename Levels, typename CanTrade, typename Sink>
    Qty match(Levels& levels, OrderId taker, Qty left, CanTrade can_trade, Sink& on_fill) {
        while (left > 0 && !levels.empty()) {
            auto best_level = levels.begin();
            if (!can_trade(best_level->first)) break;
            RestingOrder& maker = best_level->second.front();
            const Qty traded = std::min(left, maker.qty);
            on_fill(Fill{maker.id, taker, best_level->first, traded});
            left -= traded;
            maker.qty -= traded;
            if (maker.qty == 0) {
                index_.erase(maker.id);
                best_level->second.pop_front();
                if (best_level->second.empty()) levels.erase(best_level);
            }
        }
        return left;
    }

    std::map<Price, Queue, std::greater<Price>> bids_;
    std::map<Price, Queue> asks_;
    std::unordered_map<OrderId, Where> index_;
};

static_assert(OrderBook<ReferenceBook>);

}  // namespace tickforge::book
