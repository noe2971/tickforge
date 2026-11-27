#pragma once
// A memory pool for fixed-size objects (orders and price levels).
//
// Why: `new`/`delete` on every order would put the general-purpose allocator
// (locks, size classes, possible system calls) on the hot path, and would
// scatter orders around memory. The pool instead carves objects out of big
// blocks it owns and keeps released objects on a free list:
//
//   acquire(): pop the free list, or bump a pointer in the current block.
//   release(): push the object onto the free list.
//
// Both are a handful of instructions and never call the allocator. The pool
// only asks the system for memory when it has no free object left (a new
// high-water mark of live objects); reserve(n) pre-allocates so that never
// happens below n live objects. allocation_count() reports how often it did.
//
// Under AddressSanitizer, released objects are "poisoned", so code that uses
// an order after it was released is caught even though the memory stays
// inside the pool instead of going back to malloc.

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TICKFORGE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define TICKFORGE_ASAN 1
#endif
#if defined(TICKFORGE_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace tickforge::book {

template <typename T>
class ObjectPool {
    static_assert(std::is_trivially_destructible_v<T>, "pooled objects are released without running destructors");
    static_assert(sizeof(T) >= sizeof(void*), "a released object must have room for the free-list link");

public:
    explicit ObjectPool(std::size_t first_block = 64) : next_block_(first_block < 1 ? 1 : first_block) {}

    ~ObjectPool() {
#if defined(TICKFORGE_ASAN)
        for (const Block& b : blocks_) ASAN_UNPOISON_MEMORY_REGION(b.cells.get(), b.size * sizeof(Cell));
#endif
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Returns a value-initialised (zeroed) T.
    T* acquire() {
        Cell* c;
        if (free_ != nullptr) {
            c = free_;
#if defined(TICKFORGE_ASAN)
            ASAN_UNPOISON_MEMORY_REGION(c, sizeof(Cell));
#endif
            free_ = link(c);
            --free_count_;
        } else {
            if (bump_ == bump_end_) grow(next_block_);
            c = bump_++;
        }
        ++live_;
        return ::new (static_cast<void*>(c)) T{};
    }

    void release(T* p) noexcept {
        Cell* c = reinterpret_cast<Cell*>(p);
        set_link(c, free_);
        free_ = c;
        ++free_count_;
        --live_;
#if defined(TICKFORGE_ASAN)
        // Keep the free-list link readable; poison the rest of the object.
        ASAN_POISON_MEMORY_REGION(reinterpret_cast<char*>(c) + sizeof(Cell*), sizeof(Cell) - sizeof(Cell*));
#endif
    }

    // Guarantees that up to n objects can be live without allocating.
    void reserve(std::size_t n) {
        const std::size_t available = free_count_ + static_cast<std::size_t>(bump_end_ - bump_);
        if (live_ + available >= n) return;
        // Hand the unused tail of the current block to the free list so it is
        // not lost when bumping moves to the new block.
        while (bump_ != bump_end_) {
            Cell* c = bump_++;
            set_link(c, free_);
            free_ = c;
            ++free_count_;
        }
        grow(n - live_ - free_count_);
    }

    std::size_t live() const noexcept { return live_; }
    std::size_t capacity() const noexcept { return capacity_; }
    // Number of times the pool called the system allocator (blocks plus the
    // occasional growth of its list of blocks).
    std::size_t allocation_count() const noexcept { return allocations_; }

private:
    struct alignas(T) Cell {
        unsigned char bytes[sizeof(T)];
    };

    static Cell* link(Cell* c) noexcept {
        Cell* next;
        std::memcpy(&next, c->bytes, sizeof next);
        return next;
    }
    static void set_link(Cell* c, Cell* next) noexcept { std::memcpy(c->bytes, &next, sizeof next); }

    void grow(std::size_t n) {
        if (blocks_.size() == blocks_.capacity()) ++allocations_;  // the vector itself will reallocate
        blocks_.push_back(Block{std::make_unique_for_overwrite<Cell[]>(n), n});
        ++allocations_;
        bump_ = blocks_.back().cells.get();
        bump_end_ = bump_ + n;
        capacity_ += n;
        // Blocks double, capped, so a pool needs O(log n) allocations to reach
        // n objects but a small book does not reserve a huge block.
        if (next_block_ < (std::size_t{1} << 16)) next_block_ *= 2;
    }

    struct Block {
        std::unique_ptr<Cell[]> cells;
        std::size_t size;
    };
    std::vector<Block> blocks_;
    std::size_t allocations_ = 0;
    Cell* free_ = nullptr;
    Cell* bump_ = nullptr;
    Cell* bump_end_ = nullptr;
    std::size_t next_block_;
    std::size_t live_ = 0;
    std::size_t free_count_ = 0;
    std::size_t capacity_ = 0;
};

}  // namespace tickforge::book
