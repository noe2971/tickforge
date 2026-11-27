// The memory pool and the order-id hash map, tested on their own.

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

#include "tickforge/book/differential.hpp"
#include "tickforge/book/id_map.hpp"
#include "tickforge/book/object_pool.hpp"

using namespace tickforge::book;

namespace {
struct Thing {
    std::uint64_t a;
    std::uint64_t b;
};
}  // namespace

TEST(ObjectPool, AcquireGivesZeroedDistinctObjects) {
    ObjectPool<Thing> pool(4);
    std::set<Thing*> seen;
    for (int i = 0; i < 100; ++i) {
        Thing* t = pool.acquire();
        EXPECT_EQ(t->a, 0u);
        EXPECT_EQ(t->b, 0u);
        t->a = static_cast<std::uint64_t>(i);
        EXPECT_TRUE(seen.insert(t).second);
    }
    EXPECT_EQ(pool.live(), 100u);
    EXPECT_GE(pool.capacity(), 100u);
}

TEST(ObjectPool, ReleasedObjectsAreReusedWithoutGrowing) {
    ObjectPool<Thing> pool(8);
    std::vector<Thing*> v;
    for (int i = 0; i < 8; ++i) v.push_back(pool.acquire());
    const auto blocks = pool.allocation_count();
    for (Thing* t : v) pool.release(t);
    EXPECT_EQ(pool.live(), 0u);
    for (int round = 0; round < 1000; ++round) {
        Thing* t = pool.acquire();
        EXPECT_EQ(t->a, 0u);  // zeroed again on reuse
        t->a = 7;
        pool.release(t);
    }
    EXPECT_EQ(pool.allocation_count(), blocks);
}

TEST(ObjectPool, ReserveAvoidsLaterGrowth) {
    ObjectPool<Thing> pool(1);
    pool.acquire();  // a partly used block exists
    pool.reserve(5000);
    const auto blocks = pool.allocation_count();
    std::vector<Thing*> v;
    for (int i = 1; i < 5000; ++i) v.push_back(pool.acquire());
    EXPECT_EQ(pool.allocation_count(), blocks);
    EXPECT_EQ(pool.live(), 5000u);
}

TEST(IdMap, InsertFindErase) {
    IdMap<int> m;
    int x = 1, y = 2;
    EXPECT_EQ(m.find(5), nullptr);
    m.insert(5, &x);
    m.insert(0, &y);  // key 0 is an ordinary key
    EXPECT_EQ(m.find(5), &x);
    EXPECT_EQ(m.find(0), &y);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_TRUE(m.erase(5));
    EXPECT_FALSE(m.erase(5));
    EXPECT_EQ(m.find(5), nullptr);
    EXPECT_EQ(m.find(0), &y);
}

TEST(IdMap, ReserveMeansNoRehash) {
    IdMap<int> m;
    m.reserve(10000);
    const auto r = m.rehash_count();
    int v = 0;
    for (std::uint64_t k = 0; k < 10000; ++k) m.insert(k * 7919, &v);
    EXPECT_EQ(m.rehash_count(), r);
    EXPECT_EQ(m.size(), 10000u);
}

// Random inserts/erases/lookups checked against std::unordered_map, with key
// patterns that collide a lot (multiples of large powers of two, sequential).
TEST(IdMap, RandomOperationsMatchStdUnorderedMap) {
    fuzz::Rng rng(42);
    for (int pattern = 0; pattern < 3; ++pattern) {
        IdMap<int> m;
        std::unordered_map<std::uint64_t, int*> ref;
        std::vector<int> values(64);
        auto key = [&]() -> std::uint64_t {
            const std::uint64_t k = rng.below(4000);
            if (pattern == 0) return k;               // dense, sequential
            if (pattern == 1) return k << 32;          // low bits all zero
            return k * 0x9E3779B97F4A7C15ULL;          // the hash multiplier itself
        };
        for (int i = 0; i < 200000; ++i) {
            const std::uint64_t k = key();
            const auto r = rng.below(10);
            if (r < 5) {
                if (ref.count(k) == 0) {
                    int* v = &values[rng.below(values.size())];
                    m.insert(k, v);
                    ref[k] = v;
                }
            } else if (r < 8) {
                EXPECT_EQ(m.erase(k), ref.erase(k) == 1);
            } else {
                auto it = ref.find(k);
                EXPECT_EQ(m.find(k), it == ref.end() ? nullptr : it->second);
            }
            ASSERT_EQ(m.size(), ref.size());
        }
        for (const auto& [k, v] : ref) ASSERT_EQ(m.find(k), v);
    }
}

TEST(FuzzRng, IsDeterministicAndInRange) {
    fuzz::Rng a(7), b(7), c(8);
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        const auto x = a.below(10);
        EXPECT_EQ(x, b.below(10));
        EXPECT_LT(x, 10u);
        if (x != c.below(10)) differs = true;
    }
    EXPECT_TRUE(differs);
    // The first outputs for seed 1 are pinned (cross-checked against an
    // independent Python version of xoshiro256** + splitmix64), so a change to
    // the generator, which would silently change every fuzz stream, fails here.
    fuzz::Rng one(1);
    EXPECT_EQ(one.next(), 12966619160104079557ULL);
    EXPECT_EQ(one.next(), 9600361134598540522ULL);
}
