#ifndef EXSIM_BOOK_ACTIVE_BITMAP_HPP
#define EXSIM_BOOK_ACTIVE_BITMAP_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace exsim::book {

// Occupancy bitmap with summary layers. Layer 0 holds one bit per index; bit i
// of layer k + 1 is set while word i of layer k is nonzero. The top layer is a
// single word, so next/previous searches read O(log64 size) words.
class ActiveBitmap {
public:
    ActiveBitmap() = default;
    explicit ActiveBitmap(std::size_t size) {
        size_ = size;
        std::size_t word_count = size;
        do {
            word_count = (word_count + 63) / 64;
            layers_.emplace_back(word_count, 0);
        } while (word_count > 1);
    }

    void set(std::size_t index) noexcept {
        if (test(index)) return;
        ++count_;
        for (auto& words : layers_) {
            std::uint64_t& word = words[index / 64];
            const bool was_empty = word == 0;
            word |= std::uint64_t{1} << (index % 64);
            if (!was_empty) break;
            index /= 64;
        }
    }

    void clear(std::size_t index) noexcept {
        if (!test(index)) return;
        --count_;
        for (auto& words : layers_) {
            std::uint64_t& word = words[index / 64];
            word &= ~(std::uint64_t{1} << (index % 64));
            if (word != 0) break;
            index /= 64;
        }
    }

    [[nodiscard]] bool test(std::size_t index) const noexcept {
        return (layers_[0][index / 64] >> (index % 64)) & 1;
    }

    // Lowest set index >= from, or nullopt.
    [[nodiscard]] std::optional<std::size_t> find_at_or_above(std::size_t from) const noexcept {
        return from < size_ ? find_in_layer(0, from, true) : std::nullopt;
    }

    // Highest set index <= from, or nullopt.
    [[nodiscard]] std::optional<std::size_t> find_at_or_below(std::size_t from) const noexcept {
        return from < size_ ? find_in_layer(0, from, false) : std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] std::size_t word_count() const noexcept { return layers_.empty() ? 0 : layers_[0].size(); }
    [[nodiscard]] std::uint64_t word(std::size_t index) const noexcept { return layers_[0][index]; }

private:
    [[nodiscard]] std::optional<std::size_t> find_in_layer(std::size_t layer, std::size_t from,
                                                           bool ascending) const noexcept {
        const auto& words = layers_[layer];
        const std::size_t word_index = from / 64;
        if (word_index >= words.size()) return std::nullopt;
        const unsigned offset = static_cast<unsigned>(from % 64);
        const std::uint64_t in_range = ascending ? ~std::uint64_t{0} << offset : ~std::uint64_t{0} >> (63 - offset);
        if (const std::uint64_t bits = words[word_index] & in_range; bits != 0) {
            return word_index * 64 + first_bit_in_direction(bits, ascending);
        }
        if (layer + 1 == layers_.size() || (!ascending && word_index == 0)) return std::nullopt;
        // The summary layer names the nearest nonzero word in this layer.
        const auto nonzero_word =
            find_in_layer(layer + 1, ascending ? word_index + 1 : word_index - 1, ascending);
        if (!nonzero_word) return std::nullopt;
        return *nonzero_word * 64 + first_bit_in_direction(words[*nonzero_word], ascending);
    }

    [[nodiscard]] static std::size_t first_bit_in_direction(std::uint64_t bits, bool ascending) noexcept {
        return ascending ? static_cast<std::size_t>(std::countr_zero(bits))
                         : static_cast<std::size_t>(63 - std::countl_zero(bits));
    }

    std::vector<std::vector<std::uint64_t>> layers_;
    std::size_t size_ = 0;
    std::size_t count_ = 0;
};

}  // namespace exsim::book

#endif
