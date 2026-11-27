#pragma once
// Differential fuzz testing: run the same random order stream through two
// engines and demand identical answers after every single event.
//
// Why this proves correctness (as far as testing can): the ReferenceBook is
// small and written for clarity with std::map/std::list, so it is easy to
// check by reading. The FastBook is complicated (pools, intrusive lists, a
// hand-written hash map, a sorted level array). If, over millions of random
// events, the FastBook returns the same status, the same fills in the same
// order and the same complete book as the reference after every event, then
// any bug in the FastBook would have to be one that random streams never
// trigger, or one the reference shares. The stream mixes valid and invalid
// calls, many orders per price (so FIFO order matters), deep books and
// empty books.
//
// The random generator is our own (xoshiro256**) because the standard
// library's distributions differ between compilers; a seed must reproduce
// the same stream on every machine.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "tickforge/book/fast_book.hpp"
#include "tickforge/book/reference_book.hpp"
#include "tickforge/book/types.hpp"

namespace tickforge::book::fuzz {

// xoshiro256** (Blackman & Vigna), seeded through splitmix64.
class Rng {
public:
    explicit Rng(std::uint64_t seed) {
        for (auto& w : s_) {
            seed += 0x9E3779B97F4A7C15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            w = z ^ (z >> 31);
        }
    }
    std::uint64_t next() noexcept {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }
    // Uniform integer in [0, n), n > 0, without modulo bias (rejection).
    std::uint64_t below(std::uint64_t n) noexcept {
        const std::uint64_t limit = UINT64_MAX - UINT64_MAX % n;
        std::uint64_t x;
        do {
            x = next();
        } while (x >= limit);
        return x % n;
    }
    bool percent(std::uint64_t p) noexcept { return below(100) < p; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s_[4];
};

struct Config {
    std::size_t events = 100;
    std::size_t target_orders = 20;  // above this, cancels become more likely
    Price band = 5;                  // usual distance of a price from the mid, in ticks
    Price far_band = 100;            // occasional far-away prices (deep book)
    Price start_mid = 1000;
};

inline Config short_stream_config() { return Config{}; }
inline Config long_stream_config(std::size_t events) { return Config{events, 400, 50, 2000, 50000}; }

// The complete, canonical state of a book.
struct BookState {
    struct Entry {
        Price price;
        OrderId id;
        Qty qty;
        friend bool operator==(const Entry&, const Entry&) = default;
    };
    std::vector<LevelView> levels[2];  // best first
    std::vector<Entry> orders[2];      // best level first, queue order within a level
    std::size_t order_count = 0;

    bool operator==(const BookState& o) const {
        return order_count == o.order_count && levels[0] == o.levels[0] && levels[1] == o.levels[1] &&
               orders[0] == o.orders[0] && orders[1] == o.orders[1];
    }
};

template <OrderBook Book>
void capture(const Book& b, BookState& out) {
    for (Side s : {Side::buy, Side::sell}) {
        auto& lv = out.levels[index(s)];
        auto& od = out.orders[index(s)];
        lv.clear();
        od.clear();
        b.for_each_level(s, [&](const LevelView& l) { lv.push_back(l); });
        b.for_each_order(s, [&](Price p, OrderId id, Qty q) { od.push_back({p, id, q}); });
    }
    out.order_count = b.order_count();
}

enum class Op : std::uint8_t { limit = 0, market, add, execute, cancel, remove, replace };
inline constexpr std::size_t kOps = 7;
constexpr const char* to_string(Op op) noexcept {
    constexpr const char* names[] = {"limit", "market", "add", "execute", "cancel", "remove", "replace"};
    return names[static_cast<std::size_t>(op)];
}

struct Counters {
    std::uint64_t streams = 0;
    std::uint64_t events = 0;
    std::uint64_t fills = 0;
    std::uint64_t filled_shares = 0;
    std::size_t max_orders = 0;
    std::size_t max_levels = 0;
    std::uint64_t by_op[kOps][kStatusCount] = {};

    void merge(const Counters& o) {
        streams += o.streams;
        events += o.events;
        fills += o.fills;
        filled_shares += o.filled_shares;
        max_orders = std::max(max_orders, o.max_orders);
        max_levels = std::max(max_levels, o.max_levels);
        for (std::size_t i = 0; i < kOps; ++i)
            for (std::size_t j = 0; j < kStatusCount; ++j) by_op[i][j] += o.by_op[i][j];
    }
};

struct Mismatch {
    std::uint64_t seed = 0;
    std::size_t event = 0;
    std::string detail;
};

namespace detail {

inline std::string describe(const LevelView& l) {
    std::ostringstream s;
    s << "{px " << l.price << ", " << l.shares << " sh, " << l.orders << " orders}";
    return s.str();
}

inline std::string first_difference(const BookState& a, const BookState& b) {
    std::ostringstream s;
    if (a.order_count != b.order_count) s << "order_count " << a.order_count << " vs " << b.order_count << "; ";
    for (int side = 0; side < 2; ++side) {
        const char* name = side == 0 ? "bid" : "ask";
        const auto& la = a.levels[side];
        const auto& lb = b.levels[side];
        if (la.size() != lb.size()) s << name << " levels " << la.size() << " vs " << lb.size() << "; ";
        for (std::size_t i = 0; i < std::min(la.size(), lb.size()); ++i) {
            if (!(la[i] == lb[i])) {
                s << name << " level " << i << ": " << describe(la[i]) << " vs " << describe(lb[i]) << "; ";
                break;
            }
        }
        const auto& oa = a.orders[side];
        const auto& ob = b.orders[side];
        if (oa.size() != ob.size()) s << name << " orders " << oa.size() << " vs " << ob.size() << "; ";
        for (std::size_t i = 0; i < std::min(oa.size(), ob.size()); ++i) {
            if (!(oa[i] == ob[i])) {
                s << name << " order " << i << ": {px " << oa[i].price << " id " << oa[i].id << " qty " << oa[i].qty
                  << "} vs {px " << ob[i].price << " id " << ob[i].id << " qty " << ob[i].qty << "}; ";
                break;
            }
        }
    }
    return s.str();
}

}  // namespace detail

// Runs one random stream through engines A and B (A is the one under test,
// B the reference). Returns the first disagreement, or nullopt if they agreed
// after every event.
template <OrderBook A = FastBook, OrderBook B = ReferenceBook>
std::optional<Mismatch> run_stream(std::uint64_t seed, const Config& cfg, Counters& counters) {
    Rng rng(seed);
    A a;
    B b;
    std::vector<OrderId> ids;  // ids that were live at some point (may be stale)
    OrderId next_id = 1;
    Price mid = cfg.start_mid;
    std::vector<Fill> fills_a, fills_b;
    BookState state_a, state_b;
    auto sink_a = [&](const Fill& f) { fills_a.push_back(f); };
    auto sink_b = [&](const Fill& f) { fills_b.push_back(f); };
    ++counters.streams;

    auto existing_id = [&]() -> OrderId {
        const auto r = rng.below(100);
        if (r < 88 && !ids.empty()) {
            const std::size_t i = rng.below(ids.size());
            const OrderId id = ids[i];
            if (!b.find(id)) {  // stale: drop it from the list, still use it once
                ids[i] = ids.back();
                ids.pop_back();
            }
            return id;
        }
        if (r < 94) return 1 + rng.below(next_id);              // often dead, sometimes live
        return next_id + 1'000'000 + rng.below(1000);           // never used
    };
    auto new_id = [&]() -> OrderId {
        if (!ids.empty() && rng.percent(3)) return ids[rng.below(ids.size())];  // likely a duplicate
        return next_id++;
    };
    auto new_qty = [&]() -> Qty {
        const auto r = rng.below(100);
        if (r < 2) return 0;
        if (r < 7) return static_cast<Qty>(1 + rng.below(1000));
        return static_cast<Qty>(1 + rng.below(100));
    };
    auto qty_for = [&](OrderId id) -> Qty {
        const auto o = b.find(id);
        if (!o) return static_cast<Qty>(1 + rng.below(100));
        const auto r = rng.below(100);
        if (r < 60) return static_cast<Qty>(1 + rng.below(o->qty));  // partial or full
        if (r < 80) return o->qty;                                     // exactly all of it
        if (r < 88) return static_cast<Qty>(o->qty + 1 + rng.below(5));  // too many
        if (r < 90) return 0;
        return static_cast<Qty>(1 + rng.below(100));
    };
    auto price = [&]() -> Price {
        const auto r = rng.below(1000);
        if (r < 5) return -static_cast<Price>(rng.below(3));  // 0, -1 or -2: invalid
        const Price spread = r < 55 ? cfg.far_band : cfg.band;
        const Price p = mid - spread + static_cast<Price>(rng.below(static_cast<std::uint64_t>(2 * spread + 1)));
        return p < 1 ? 1 : p;
    };
    auto side = [&]() { return rng.below(2) == 0 ? Side::buy : Side::sell; };

    for (std::size_t ev = 0; ev < cfg.events; ++ev) {
        if (rng.percent(10)) {
            mid += static_cast<Price>(rng.below(3)) - 1;
            if (mid < cfg.far_band + 10) mid = cfg.far_band + 10;
        }
        const bool crowded = b.order_count() > cfg.target_orders;
        // Weights (out of 100) for limit, market, add, execute, cancel, remove, replace.
        static constexpr std::uint64_t kCalm[kOps] = {35, 5, 20, 10, 8, 12, 10};
        static constexpr std::uint64_t kCrowded[kOps] = {20, 5, 10, 15, 15, 25, 10};
        const auto* w = crowded ? kCrowded : kCalm;
        std::uint64_t pick = rng.below(100);
        std::size_t op_index = 0;
        while (pick >= w[op_index]) pick -= w[op_index++];
        const Op op = static_cast<Op>(op_index);

        fills_a.clear();
        fills_b.clear();
        std::ostringstream what;
        bool same = true;
        Status status = Status::ok;

        switch (op) {
            case Op::limit: {
                const OrderId id = new_id();
                const Side s = side();
                const Price p = price();
                const Qty q = new_qty();
                what << "limit " << (s == Side::buy ? "buy" : "sell") << " id " << id << " px " << p << " qty " << q;
                const auto ra = a.submit_limit(id, s, p, q, sink_a);
                const auto rb = b.submit_limit(id, s, p, q, sink_b);
                same = ra == rb && fills_a == fills_b;
                status = rb.status;
                if (rb.status == Status::ok && rb.rested > 0) ids.push_back(id);
                break;
            }
            case Op::market: {
                const OrderId id = new_id();
                const Side s = side();
                const Qty q = static_cast<Qty>(rng.percent(2) ? 0 : 1 + rng.below(300));
                what << "market " << (s == Side::buy ? "buy" : "sell") << " id " << id << " qty " << q;
                const auto ra = a.submit_market(id, s, q, sink_a);
                const auto rb = b.submit_market(id, s, q, sink_b);
                same = ra == rb && fills_a == fills_b;
                status = rb.status;
                break;
            }
            case Op::add: {
                const OrderId id = new_id();
                const Side s = side();
                const Price p = price();
                const Qty q = new_qty();
                what << "add " << (s == Side::buy ? "buy" : "sell") << " id " << id << " px " << p << " qty " << q;
                const auto ra = a.add(id, s, p, q);
                const auto rb = b.add(id, s, p, q);
                same = ra == rb;
                status = rb;
                if (rb == Status::ok) ids.push_back(id);
                break;
            }
            case Op::execute: {
                const OrderId id = existing_id();
                const Qty q = qty_for(id);
                what << "execute id " << id << " qty " << q;
                const auto ra = a.execute(id, q);
                const auto rb = b.execute(id, q);
                same = ra == rb;
                status = rb.status;
                break;
            }
            case Op::cancel: {
                const OrderId id = existing_id();
                const Qty q = qty_for(id);
                what << "cancel id " << id << " qty " << q;
                const auto ra = a.cancel(id, q);
                const auto rb = b.cancel(id, q);
                same = ra == rb;
                status = rb;
                break;
            }
            case Op::remove: {
                const OrderId id = existing_id();
                what << "remove id " << id;
                const auto ra = a.remove(id);
                const auto rb = b.remove(id);
                same = ra == rb;
                status = rb;
                break;
            }
            case Op::replace: {
                const OrderId old_id = existing_id();
                const OrderId id = rng.percent(2) ? old_id : new_id();
                const Price p = price();
                const Qty q = new_qty();
                what << "replace id " << old_id << " -> " << id << " px " << p << " qty " << q;
                const auto ra = a.replace(old_id, id, p, q);
                const auto rb = b.replace(old_id, id, p, q);
                same = ra == rb;
                status = rb;
                if (rb == Status::ok) ids.push_back(id);
                break;
            }
        }

        ++counters.events;
        ++counters.by_op[op_index][static_cast<std::size_t>(status)];
        counters.fills += fills_b.size();
        for (const Fill& f : fills_b) counters.filled_shares += f.qty;

        if (!same) {
            std::ostringstream d;
            d << what.str() << ": engines returned different results or fills (fast " << fills_a.size()
              << " fills, reference " << fills_b.size() << ")";
            return Mismatch{seed, ev, d.str()};
        }
        capture(a, state_a);
        capture(b, state_b);
        if (!(state_a == state_b)) {
            return Mismatch{seed, ev, what.str() + ": books differ: " + detail::first_difference(state_a, state_b)};
        }
        counters.max_orders = std::max(counters.max_orders, state_b.order_count);
        counters.max_levels = std::max(counters.max_levels, state_b.levels[0].size() + state_b.levels[1].size());
    }
    return std::nullopt;
}

}  // namespace tickforge::book::fuzz
