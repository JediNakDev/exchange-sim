#ifndef LOB_FLAT_HASH_MAP_HPP
#define LOB_FLAT_HASH_MAP_HPP

#include "compiler.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <stdexcept>
#include <utility>
#include <type_traits>

namespace lob {

/**
 * FlatHashMap - open-addressing hash map for non-zero uint64 keys.
 *
 * - Key 0 is the empty-slot sentinel and may not be inserted.
 * - Linear probing over a power-of-two table, Fibonacci hashing.
 * - Backward-shift deletion: no tombstones, so probe lengths stay short under
 *   the insert/erase churn of an order-id workload (most orders are cancelled).
 * - All memory is allocated (and pre-faulted) by reserve(). insert() never
 *   allocates unless growth was explicitly enabled; when the map is full and
 *   growth is disabled, insert() fails instead.
 */
template <typename V>
class FlatHashMap {
    static_assert(std::is_trivially_copyable_v<V>, "FlatHashMap values must be trivially copyable");

public:
    using Key = std::uint64_t;

    FlatHashMap() = default;
    explicit FlatHashMap(std::size_t max_entries) { reserve(max_entries); }

    FlatHashMap(const FlatHashMap&) = delete;
    FlatHashMap& operator=(const FlatHashMap&) = delete;
    FlatHashMap(FlatHashMap&& other) noexcept { swap(other); }
    FlatHashMap& operator=(FlatHashMap&& other) noexcept {
        if (this != &other) {
            FlatHashMap moved(std::move(other));
            swap(moved);
        }
        return *this;
    }

    // Sizes the table to hold at least `max_entries` keys at <= 50% load.
    // Existing entries are preserved. Never shrinks.
    void reserve(std::size_t max_entries) {
        if (max_entries <= max_entries_) {
            return;
        }
        const auto limit = std::numeric_limits<std::size_t>::max();
        if (max_entries > limit / 2) throw std::length_error("FlatHashMap reserve");
        std::size_t capacity = 16;
        while (capacity < max_entries * 2) {
            if (capacity > limit / 2) throw std::length_error("FlatHashMap reserve");
            capacity *= 2;
        }
        if (capacity > limit / sizeof(Slot)) throw std::length_error("FlatHashMap storage");
        rehash(capacity);
    }

    void set_allow_growth(bool allow_growth) noexcept { allow_growth_ = allow_growth; }

    [[nodiscard]] V* find(Key key) noexcept {
        if (LOB_UNLIKELY(mask_ == 0 || key == 0)) {
            return nullptr;
        }
        for (std::size_t i = slot_of(key);; i = (i + 1) & mask_) {
            Slot& slot = slots_[i];
            if (slot.key == key) {
                return &slot.value;
            }
            if (slot.key == 0) {
                return nullptr;
            }
        }
    }

    [[nodiscard]] const V* find(Key key) const noexcept {
        return const_cast<FlatHashMap*>(this)->find(key);
    }

    [[nodiscard]] bool contains(Key key) const noexcept { return find(key) != nullptr; }

    // Inserts key -> value. Returns false if the key already exists, the key
    // is 0, or the map is full and growth is disabled.
    [[nodiscard]] bool insert(Key key, const V& value) {
        if (LOB_UNLIKELY(key == 0)) {
            return false;
        }
        if (LOB_UNLIKELY(size_ >= max_entries_)) {
            if (!allow_growth_) {
                return false;
            }
            reserve(max_entries_ == 0 ? 8 : max_entries_ + 1);
        }
        for (std::size_t i = slot_of(key);; i = (i + 1) & mask_) {
            Slot& slot = slots_[i];
            if (slot.key == 0) {
                slot.key = key;
                slot.value = value;
                ++size_;
                return true;
            }
            if (slot.key == key) {
                return false;
            }
        }
    }

    // Removes `key`. Returns false if it was not present.
    bool erase(Key key) noexcept {
        if (LOB_UNLIKELY(mask_ == 0 || key == 0)) {
            return false;
        }
        std::size_t hole = slot_of(key);
        while (slots_[hole].key != key) {
            if (slots_[hole].key == 0) {
                return false;
            }
            hole = (hole + 1) & mask_;
        }
        // Backward-shift: pull later members of the probe run into the hole
        // whenever the hole lies on their probe path.
        for (std::size_t j = (hole + 1) & mask_;; j = (j + 1) & mask_) {
            const Key k = slots_[j].key;
            if (k == 0) {
                break;
            }
            const std::size_t ideal = slot_of(k);
            if (((j - ideal) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = 0;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (std::size_t i = 0; mask_ != 0 && i <= mask_; ++i) {
            slots_[i].key = 0;
        }
        size_ = 0;
    }

    // Visits every (key, value). Must not insert or erase during iteration.
    template <typename F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; mask_ != 0 && i <= mask_; ++i) {
            const Slot& slot = slots_[i];
            if (slot.key != 0) {
                f(slot.key, slot.value);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ == 0 ? 0 : mask_ + 1; }

private:
    void swap(FlatHashMap& other) noexcept {
        using std::swap;
        swap(slots_, other.slots_);
        swap(mask_, other.mask_);
        swap(size_, other.size_);
        swap(max_entries_, other.max_entries_);
        swap(shift_, other.shift_);
        swap(allow_growth_, other.allow_growth_);
    }

    struct Slot {
        Key key;
        V value;
    };

    [[nodiscard]] std::size_t slot_of(Key key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void rehash(std::size_t new_capacity) {
        auto replacement = std::make_unique<Slot[]>(new_capacity);
        std::unique_ptr<Slot[]> old = std::move(slots_);
        const std::size_t old_capacity = capacity();

        // Value-initialisation zero-fills every slot, which also pre-faults
        // the pages so the hot path never takes a first-touch page fault.
        slots_ = std::move(replacement);
        mask_ = new_capacity - 1;
        max_entries_ = new_capacity / 2;
        shift_ = 64;
        for (std::size_t c = new_capacity; c > 1; c >>= 1) {
            --shift_;
        }
        size_ = 0;

        for (std::size_t i = 0; i < old_capacity; ++i) {
            if (old[i].key != 0) {
                for (std::size_t s = slot_of(old[i].key);; s = (s + 1) & mask_) {
                    if (slots_[s].key == 0) {
                        slots_[s] = old[i];
                        ++size_;
                        break;
                    }
                }
            }
        }
    }

    std::unique_ptr<Slot[]> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t max_entries_ = 0;
    unsigned shift_ = 64;
    bool allow_growth_ = false;
};

}  // namespace lob

#endif
