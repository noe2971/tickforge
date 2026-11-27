#pragma once
// FastBook: the limit order book used for replay, validation and benchmarks.
//
// Data structures (see docs/order_book_design.md for the full reasoning):
//
//   Order  — one resting order. Orders at the same price form an *intrusive*
//            doubly linked list: the prev/next pointers live inside the Order
//            itself, so joining or leaving the queue is a few pointer writes
//            and needs no separate list node. The order also points to its
//            Level, so execute/cancel/delete never search for the price.
//
//   Level  — one price on one side: head/tail of its order queue (head = the
//            oldest order = first to trade), total shares and order count.
//
//   Level array (one per side) — a flat std::vector of {key, Level*} kept
//            sorted from worst price to best, so the best price is always
//            back(): best bid/ask is O(1), executions at the touch pop from
//            the back, and a new level near the touch only shifts the few
//            entries behind it. Finding an existing price is a binary search
//            over contiguous memory. key = price for bids, -price for asks,
//            so "ascending key" means "worse to better" on both sides.
//
//   IdMap  — open-addressing hash map from order id to Order*, for O(1)
//            execute/cancel/delete/replace by id.
//
//   ObjectPool<Order>, ObjectPool<Level> — every Order and Level comes from a
//            pool, so the hot path never calls malloc/free once the pools,
//            the id map and the level arrays have reached their high-water
//            marks (reserve() sets them in advance).
//
// Replay mode (add/execute/cancel/remove/replace) applies what the exchange
// says happened and never matches. Matching mode (submit_limit /
// submit_market) does strict price-time priority matching on the same book.
//
// Checks happen before anything changes, in this order, identically in the
// ReferenceBook: id (unknown / duplicate), then shares, then price. A call
// that returns an error leaves the book untouched.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "tickforge/book/id_map.hpp"
#include "tickforge/book/object_pool.hpp"
#include "tickforge/book/types.hpp"

namespace tickforge::book {

class FastBook {
public:
    FastBook() : order_pool_(64), level_pool_(16), ids_(16) {}
    FastBook(const FastBook&) = delete;
    FastBook& operator=(const FastBook&) = delete;

    // Pre-sizes everything so that up to `orders` live orders and
    // `levels_per_side` price levels on each side never allocate.
    void reserve(std::size_t orders, std::size_t levels_per_side) {
        order_pool_.reserve(orders);
        ids_.reserve(orders);
        level_pool_.reserve(2 * levels_per_side);
        for (auto& side : sides_)
            if (side.capacity() < levels_per_side) side.reserve(levels_per_side);
    }

    // ---- Replay mode ---------------------------------------------------------

    Status add(OrderId id, Side side, Price price, Qty qty) {
        if (ids_.contains(id)) return Status::duplicate_order;
        if (qty == 0) return Status::zero_quantity;
        if (price <= 0) return Status::bad_price;
        insert_order(id, side, price, qty);
        return Status::ok;
    }

    // The order traded `qty` shares at its resting price.
    ExecResult execute(OrderId id, Qty qty) { return reduce(id, qty); }

    // Partial cancel: removes `qty` shares, keeps time priority. Cancelling
    // every remaining share removes the order.
    Status cancel(OrderId id, Qty qty) { return reduce(id, qty).status; }

    // Full cancel / delete.
    Status remove(OrderId id) {
        Order* o = ids_.find(id);
        if (o == nullptr) return Status::unknown_order;
        unlink(o);
        return Status::ok;
    }

    // ITCH-style replace: the old order leaves the book and a new order with
    // a new id, the same side, the new price and size joins the back of its
    // queue (it loses time priority). Never matches.
    Status replace(OrderId old_id, OrderId new_id, Price price, Qty qty) {
        Order* o = ids_.find(old_id);
        if (o == nullptr) return Status::unknown_order;
        if (new_id == old_id || ids_.contains(new_id)) return Status::duplicate_order;
        if (qty == 0) return Status::zero_quantity;
        if (price <= 0) return Status::bad_price;
        const Side side = o->level->side;
        unlink(o);  // first, so live counts never exceed their peak
        insert_order(new_id, side, price, qty);
        return Status::ok;
    }

    // ---- Matching mode -------------------------------------------------------

    // Trades against the opposite side while prices cross (best price first,
    // oldest order first within a price), then rests any remainder at `price`.
    // on_fill is called once per trade, before the book changes for that
    // trade; it must not call back into this book.
    template <FillSink Sink>
    SubmitResult submit_limit(OrderId id, Side side, Price price, Qty qty, Sink&& on_fill) {
        if (ids_.contains(id)) return {Status::duplicate_order, 0, 0};
        if (qty == 0) return {Status::zero_quantity, 0, 0};
        if (price <= 0) return {Status::bad_price, 0, 0};
        const Qty left = match<true>(id, side, price, qty, on_fill);
        if (left > 0) insert_order(id, side, price, left);
        return {Status::ok, qty - left, left};
    }

    // Trades against the opposite side until filled or the side is empty; any
    // remainder is cancelled (a market order never rests).
    template <FillSink Sink>
    SubmitResult submit_market(OrderId id, Side side, Qty qty, Sink&& on_fill) {
        if (ids_.contains(id)) return {Status::duplicate_order, 0, 0};
        if (qty == 0) return {Status::zero_quantity, 0, 0};
        const Qty left = match<false>(id, side, 0, qty, on_fill);
        return {Status::ok, qty - left, 0};
    }

    // ---- Queries ---------------------------------------------------------------

    std::optional<LevelView> best(Side side) const noexcept {
        const auto& levels = sides_[index(side)];
        if (levels.empty()) return std::nullopt;
        return view(*levels.back().level);
    }

    // Writes up to n levels, best first; returns how many were written.
    std::size_t depth(Side side, LevelView* out, std::size_t n) const noexcept {
        const auto& levels = sides_[index(side)];
        std::size_t k = 0;
        for (auto it = levels.rbegin(); it != levels.rend() && k < n; ++it) out[k++] = view(*it->level);
        return k;
    }

    std::optional<OrderView> find(OrderId id) const noexcept {
        const Order* o = ids_.find(id);
        if (o == nullptr) return std::nullopt;
        return OrderView{id, o->level->side, o->level->price, o->qty, o->prev == nullptr};
    }

    std::size_t order_count() const noexcept { return ids_.size(); }
    std::size_t level_count(Side side) const noexcept { return sides_[index(side)].size(); }

    // f(const LevelView&) for every level, best first.
    template <typename F>
    void for_each_level(Side side, F&& f) const {
        const auto& levels = sides_[index(side)];
        for (auto it = levels.rbegin(); it != levels.rend(); ++it) f(view(*it->level));
    }

    // f(Price, OrderId, Qty) for every order: best level first, and within a
    // level in time priority (the order that would trade first comes first).
    template <typename F>
    void for_each_order(Side side, F&& f) const {
        const auto& levels = sides_[index(side)];
        for (auto it = levels.rbegin(); it != levels.rend(); ++it)
            for (const Order* o = it->level->head; o != nullptr; o = o->next) f(it->level->price, o->id, o->qty);
    }

    // How many times this book asked the system allocator for memory.
    struct MemoryStats {
        std::size_t order_blocks = 0;
        std::size_t level_blocks = 0;
        std::size_t id_map_rehashes = 0;
        std::size_t level_array_growths = 0;
        std::size_t total() const noexcept { return order_blocks + level_blocks + id_map_rehashes + level_array_growths; }
    };
    MemoryStats memory_stats() const noexcept {
        return {order_pool_.allocation_count(), level_pool_.allocation_count(), ids_.rehash_count(), level_array_growths_};
    }

private:
    struct Level;
    struct Order {
        Order* prev = nullptr;  // towards the front of the queue (older)
        Order* next = nullptr;  // towards the back of the queue (newer)
        Level* level = nullptr;
        OrderId id = 0;
        Qty qty = 0;  // remaining shares
    };
    struct Level {
        Order* head = nullptr;  // oldest order: trades first
        Order* tail = nullptr;  // newest order: new orders join here
        std::uint64_t shares = 0;
        std::uint32_t orders = 0;
        Side side = Side::buy;
        Price price = 0;
    };
    struct Slot {
        std::int64_t key;  // price for bids, -price for asks
        Level* level;
    };
    using LevelArray = std::vector<Slot>;  // ascending key: worst ... best

    static std::int64_t key_of(Side side, Price price) noexcept { return side == Side::buy ? price : -price; }
    static LevelView view(const Level& l) noexcept { return {l.price, l.shares, l.orders}; }

    static LevelArray::iterator lower_bound(LevelArray& levels, std::int64_t key) noexcept {
        return std::lower_bound(levels.begin(), levels.end(), key,
                                [](const Slot& s, std::int64_t k) { return s.key < k; });
    }

    // The level for (side, price), created if it does not exist yet.
    Level* level_for(Side side, Price price) {
        LevelArray& levels = sides_[index(side)];
        const std::int64_t key = key_of(side, price);
        // Fast paths: most adds join the best price or improve on it.
        if (levels.empty() || key > levels.back().key) {
            Level* l = new_level(side, price);
            push(levels, levels.end(), l, key);
            return l;
        }
        if (key == levels.back().key) return levels.back().level;
        auto it = lower_bound(levels, key);
        if (it->key == key) return it->level;
        Level* l = new_level(side, price);
        push(levels, it, l, key);
        return l;
    }

    Level* new_level(Side side, Price price) {
        Level* l = level_pool_.acquire();
        l->side = side;
        l->price = price;
        return l;
    }

    void push(LevelArray& levels, LevelArray::iterator pos, Level* l, std::int64_t key) {
        const std::size_t cap = levels.capacity();
        levels.insert(pos, Slot{key, l});
        if (levels.capacity() != cap) ++level_array_growths_;
    }

    void remove_level(Level* l) noexcept {
        LevelArray& levels = sides_[index(l->side)];
        if (levels.back().level == l) {
            levels.pop_back();  // the common case: the best level emptied
        } else {
            levels.erase(lower_bound(levels, key_of(l->side, l->price)));
        }
        level_pool_.release(l);
    }

    void insert_order(OrderId id, Side side, Price price, Qty qty) {
        Level* l = level_for(side, price);
        Order* o = order_pool_.acquire();
        o->id = id;
        o->qty = qty;
        o->level = l;
        // Join the back of the queue.
        o->prev = l->tail;
        o->next = nullptr;
        if (l->tail != nullptr) {
            l->tail->next = o;
        } else {
            l->head = o;
        }
        l->tail = o;
        l->shares += qty;
        ++l->orders;
        ids_.insert(id, o);
    }

    // Takes the order out of its queue, frees it, and frees its level if empty.
    void unlink(Order* o) noexcept {
        Level* l = o->level;
        if (o->prev != nullptr) {
            o->prev->next = o->next;
        } else {
            l->head = o->next;
        }
        if (o->next != nullptr) {
            o->next->prev = o->prev;
        } else {
            l->tail = o->prev;
        }
        l->shares -= o->qty;
        --l->orders;
        if (l->orders == 0) remove_level(l);
        ids_.erase(o->id);
        order_pool_.release(o);
    }

    // Shared by execute and partial cancel: same effect on the book.
    ExecResult reduce(OrderId id, Qty qty) {
        Order* o = ids_.find(id);
        if (o == nullptr) return {Status::unknown_order, Side::buy, 0, 0};
        Level* l = o->level;
        ExecResult r{Status::ok, l->side, l->price, o->qty};
        if (qty == 0) {
            r.status = Status::zero_quantity;
            return r;
        }
        if (qty > o->qty) {
            r.status = Status::too_many_shares;
            return r;
        }
        r.remaining = o->qty - qty;
        if (r.remaining == 0) {
            unlink(o);
        } else {
            o->qty = r.remaining;
            l->shares -= qty;
        }
        return r;
    }

    template <bool kHasLimit, typename Sink>
    Qty match(OrderId taker, Side side, Price limit, Qty qty, Sink& on_fill) {
        LevelArray& resting = sides_[index(opposite(side))];
        while (qty > 0 && !resting.empty()) {
            Level* l = resting.back().level;  // best opposite price
            if (kHasLimit && !crosses(side, limit, l->price)) break;
            Order* maker = l->head;  // oldest order at that price
            const Qty traded = maker->qty < qty ? maker->qty : qty;
            on_fill(Fill{maker->id, taker, l->price, traded});
            qty -= traded;
            if (traded == maker->qty) {
                unlink(maker);
            } else {
                maker->qty -= traded;
                l->shares -= traded;
            }
        }
        return qty;
    }

    LevelArray sides_[2];
    ObjectPool<Order> order_pool_;
    ObjectPool<Level> level_pool_;
    IdMap<Order> ids_;
    std::size_t level_array_growths_ = 0;
};

static_assert(OrderBook<FastBook>);

}  // namespace tickforge::book
