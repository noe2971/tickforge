// Proves the claim "the hot path never allocates": global operator new is
// replaced in this test program with a counting version, and a long burst of
// adds, executes, cancels, deletes, replaces and matching runs on a reserved
// FastBook with the counter checked before and after.
//
// This lives in its own executable because replacing operator new affects
// every test in the program.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <vector>

#include "tickforge/book/differential.hpp"
#include "tickforge/book/fast_book.hpp"
#include "tickforge/book/reference_book.hpp"

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace tickforge::book;

namespace {

// Drives a book with a realistic mix of every operation, keeping roughly
// `live_target` orders alive across ~`levels` prices.
template <typename Book>
std::uint64_t run_burst(Book& book, std::size_t events, std::size_t live_target, Price levels,
                        std::vector<OrderId>& live, OrderId& next_id) {
    fuzz::Rng rng(5);
    std::uint64_t fills = 0;
    auto sink = [&](const Fill&) { ++fills; };
    for (std::size_t i = 0; i < events; ++i) {
        const Side s = rng.below(2) == 0 ? Side::buy : Side::sell;
        // Bids below 10000, asks above, so replay adds never cross.
        const Price off = 1 + static_cast<Price>(rng.below(static_cast<std::uint64_t>(levels)));
        const Price p = s == Side::buy ? 10000 - off : 10000 + off;
        const auto r = rng.below(100);
        // Below the target: always add. Above it: adds (25%) lose to the
        // removals, so the book hovers around the target size.
        if (live.size() < live_target || r < 25) {
            const OrderId id = next_id++;
            if (book.add(id, s, p, static_cast<Qty>(1 + rng.below(500))) == Status::ok) live.push_back(id);
        } else {
            const std::size_t k = rng.below(live.size());
            const OrderId id = live[k];
            bool gone = false;
            if (r < 45) {
                gone = book.execute(id, 1 + static_cast<Qty>(rng.below(300))).remaining == 0;
            } else if (r < 55) {
                book.cancel(id, 1);
                gone = !book.find(id).has_value();
            } else if (r < 85) {
                gone = book.remove(id) == Status::ok;
            } else if (r < 95) {
                const OrderId nid = next_id++;
                if (book.replace(id, nid, p, static_cast<Qty>(1 + rng.below(500))) == Status::ok) live[k] = nid;
            } else {
                // Matching mode: an aggressive order that trades and may rest.
                const Price limit = s == Side::buy ? 10000 + 2 : 10000 - 2;
                const OrderId nid = next_id++;
                if (book.submit_limit(nid, s, limit, 50, sink).rested > 0) book.remove(nid);
            }
            if (gone || !book.find(live[k]).has_value()) {
                live[k] = live.back();
                live.pop_back();
            }
        }
    }
    return fills;
}

}  // namespace

TEST(HotPathAllocations, ReservedFastBookNeverAllocates) {
    FastBook book;
    book.reserve(5000, 1000);
    std::vector<OrderId> live;
    live.reserve(200000);
    OrderId next_id = 1;
    // Warm up once so every structure has reached its working size.
    run_burst(book, 50000, 2000, 400, live, next_id);

    const std::uint64_t before = g_allocations.load();
    const std::uint64_t fills = run_burst(book, 1'000'000, 2000, 400, live, next_id);
    const std::uint64_t after = g_allocations.load();

    EXPECT_EQ(after - before, 0u) << "heap allocations inside 1,000,000 book operations";
    EXPECT_GT(fills, 0u);
    EXPECT_GT(book.order_count(), 1000u);
}

TEST(HotPathAllocations, UnreservedFastBookOnlyAllocatesWhenGrowing) {
    FastBook book;
    std::vector<OrderId> live;
    live.reserve(200000);
    OrderId next_id = 1;
    const std::uint64_t before = g_allocations.load();
    run_burst(book, 1'000'000, 2000, 400, live, next_id);
    const std::uint64_t used = g_allocations.load() - before;
    // Every allocation is a growth step the book counted itself, and there
    // are only a few dozen of them for a million operations.
    EXPECT_EQ(used, book.memory_stats().total());
    EXPECT_LT(used, 100u);
}

TEST(HotPathAllocations, ReferenceBookAllocatesOnEveryAdd) {
    ReferenceBook book;
    book.reserve(5000, 1000);
    std::vector<OrderId> live;
    live.reserve(200000);
    OrderId next_id = 1;
    run_burst(book, 50000, 2000, 400, live, next_id);
    const std::uint64_t before = g_allocations.load();
    run_burst(book, 100000, 2000, 400, live, next_id);
    // std::list and std::unordered_map allocate a node per order.
    EXPECT_GT(g_allocations.load() - before, 50000u);
}
