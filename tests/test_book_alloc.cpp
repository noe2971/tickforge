// Proves the claim "the hot path never allocates": global operator new is
// replaced in this test program with a counting version, and a long burst of
// adds, executes, cancels, deletes, replaces and matching runs on a reserved
// FastBook with the counter checked before and after.
//
// This lives in its own executable because replacing operator new affects
// every test in the program.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "tickforge/book/differential.hpp"
#include "tickforge/book/fast_book.hpp"
#include "tickforge/book/reference_book.hpp"

// Every replaceable form is replaced (plain, array, nothrow, aligned, sized),
// so every allocation in the program is counted and every block is released
// by the same allocator that made it. Leaving a form out let the sanitizer
// or the standard library serve it, which the counter could not see and
// which AddressSanitizer on Linux reported as alloc-dealloc-mismatch.
namespace {
std::atomic<std::uint64_t> g_allocations{0};

void* counted_alloc(std::size_t n) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}

void* counted_aligned_alloc(std::size_t n, std::align_val_t al) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    std::size_t a = static_cast<std::size_t>(al);
    if (a < sizeof(void*)) a = sizeof(void*);  // posix_memalign's minimum
    void* p = nullptr;
    return posix_memalign(&p, a, n == 0 ? 1 : n) == 0 ? p : nullptr;  // released with free()
}
}  // namespace

void* operator new(std::size_t n) {
    if (void* p = counted_alloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = counted_alloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new(std::size_t n, std::align_val_t al) {
    if (void* p = counted_aligned_alloc(n, al)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t al) {
    if (void* p = counted_aligned_alloc(n, al)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return counted_aligned_alloc(n, al);
}
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return counted_aligned_alloc(n, al);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

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

// Regression test for the CI failure on Ubuntu + AddressSanitizer: every form
// of the global allocation functions must go through the counting
// replacement above. A form left out is served by another allocator (the
// sanitizer's or the standard library's): the counter misses it, and its
// memory can be released by our free()-based operator delete, which
// AddressSanitizer reports as alloc-dealloc-mismatch.
TEST(HotPathAllocations, EveryAllocationFormGoesThroughTheCounter) {
    constexpr std::size_t n = 24;
    constexpr std::align_val_t al{64};
    struct Form {
        const char* name;
        void* (*alloc)();
        void (*release)(void*);
        bool aligned;
    };
    const Form forms[] = {
        {"new / delete", [] { return ::operator new(n); }, [](void* p) { ::operator delete(p); }, false},
        {"new / sized delete", [] { return ::operator new(n); }, [](void* p) { ::operator delete(p, n); }, false},
        {"new[] / delete[]", [] { return ::operator new[](n); }, [](void* p) { ::operator delete[](p); }, false},
        {"new[] / sized delete[]", [] { return ::operator new[](n); }, [](void* p) { ::operator delete[](p, n); }, false},
        {"nothrow new", [] { return ::operator new(n, std::nothrow); },
         [](void* p) { ::operator delete(p, std::nothrow); }, false},
        {"nothrow new[]", [] { return ::operator new[](n, std::nothrow); },
         [](void* p) { ::operator delete[](p, std::nothrow); }, false},
        {"aligned new", [] { return ::operator new(n, al); }, [](void* p) { ::operator delete(p, al); }, true},
        {"aligned new / sized delete", [] { return ::operator new(n, al); },
         [](void* p) { ::operator delete(p, n, al); }, true},
        {"aligned new[]", [] { return ::operator new[](n, al); }, [](void* p) { ::operator delete[](p, al); }, true},
        {"aligned new[] / sized delete[]", [] { return ::operator new[](n, al); },
         [](void* p) { ::operator delete[](p, n, al); }, true},
        {"aligned nothrow new", [] { return ::operator new(n, al, std::nothrow); },
         [](void* p) { ::operator delete(p, al, std::nothrow); }, true},
        {"aligned nothrow new[]", [] { return ::operator new[](n, al, std::nothrow); },
         [](void* p) { ::operator delete[](p, al, std::nothrow); }, true},
    };
    for (const Form& f : forms) {
        const std::uint64_t before = g_allocations.load();
        void* p = f.alloc();
        ASSERT_NE(p, nullptr) << f.name;
        EXPECT_EQ(g_allocations.load() - before, 1u) << f.name << " did not go through the counting operator new";
        if (f.aligned) EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64, 0u) << f.name;
        f.release(p);
    }
}

// The exact path that failed in CI: std::stable_sort takes a temporary
// buffer from the nothrow operator new and gives it back with operator
// delete. Under AddressSanitizer with the old, incomplete replacement this
// aborted the program on Linux.
TEST(HotPathAllocations, StableSortTemporaryBufferUsesMatchingAllocator) {
    std::vector<int> v(10000);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int>((i * 7919) % 1000);
    std::stable_sort(v.begin(), v.end());
    EXPECT_TRUE(std::is_sorted(v.begin(), v.end()));
}
