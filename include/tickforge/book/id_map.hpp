#pragma once
// Hash map from a 64-bit order reference to a pointer (order id -> Order*).
//
// Why not std::unordered_map: it allocates a separate heap node for every
// insert and chases a pointer per lookup. This map is one flat array of
// {key, value} slots ("open addressing"):
//
//   * lookup: hash the key to a slot, then walk forward until the key or an
//     empty slot is found ("linear probing"). A slot is 16 bytes, so one
//     64-byte cache line holds four and a short probe run stays in one or two.
//   * hash: Fibonacci hashing, key * 2^64/phi, keeping the top bits. ITCH
//     order references are nearly sequential, and this spreads them evenly.
//   * load factor <= 1/2, so probe runs stay short.
//   * erase: "backward-shift deletion" moves later entries of the same run
//     back into the hole, so no tombstones pile up and lookups stay fast
//     after millions of deletes.
//
// A null value marks an empty slot, so null cannot be stored. The array only
// grows (a rehash, counted by rehash_count()) when the map passes half full;
// reserve(n) sizes it so n entries never trigger one.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace tickforge::book {

template <typename V>
class IdMap {
public:
    explicit IdMap(std::size_t initial_capacity = 16) { resize(std::bit_ceil(initial_capacity < 16 ? 16 : initial_capacity)); }

    V* find(std::uint64_t key) const noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Entry& e = slots_[i];
            if (e.value == nullptr) return nullptr;
            if (e.key == key) return e.value;
        }
    }

    bool contains(std::uint64_t key) const noexcept { return find(key) != nullptr; }

    // Inserts key -> value. Precondition: key is not present, value != nullptr.
    void insert(std::uint64_t key, V* value) {
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        place(key, value);
        ++size_;
    }

    // Removes key; returns false if it was not present.
    bool erase(std::uint64_t key) noexcept {
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].value == nullptr) return false;
            if (slots_[i].key == key) break;
        }
        // Backward-shift: pull later entries of this probe run into the hole,
        // unless an entry's home slot lies in (hole, its position] — moving it
        // before its home would make it unfindable.
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].value == nullptr) break;
            const std::size_t k = home(slots_[j].key);
            const bool home_in_between = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (home_in_between) continue;
            slots_[i] = slots_[j];
            i = j;
        }
        slots_[i] = Entry{};
        --size_;
        return true;
    }

    // Guarantees that up to n entries fit without a rehash.
    void reserve(std::size_t n) {
        const std::size_t need = std::bit_ceil(n * 2 < 16 ? std::size_t{16} : n * 2);
        if (need > slots_.size()) rehash(need);
    }

    void clear() noexcept {
        for (auto& e : slots_) e = Entry{};
        size_ = 0;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }
    std::size_t rehash_count() const noexcept { return rehashes_; }

private:
    struct Entry {
        std::uint64_t key = 0;
        V* value = nullptr;
    };

    std::size_t home(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    void place(std::uint64_t key, V* value) noexcept {
        std::size_t i = home(key);
        while (slots_[i].value != nullptr) i = (i + 1) & mask_;
        slots_[i] = Entry{key, value};
    }

    void resize(std::size_t cap) {
        slots_.assign(cap, Entry{});
        mask_ = cap - 1;
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(cap));
    }

    void rehash(std::size_t cap) {
        std::vector<Entry> old = std::move(slots_);
        resize(cap);
        for (const Entry& e : old)
            if (e.value != nullptr) place(e.key, e.value);
        ++rehashes_;
    }

    std::vector<Entry> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
    std::size_t rehashes_ = 0;
};

}  // namespace tickforge::book
