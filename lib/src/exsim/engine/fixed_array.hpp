#ifndef EXSIM_ENGINE_FIXED_ARRAY_HPP
#define EXSIM_ENGINE_FIXED_ARRAY_HPP

#include <cstddef>
#include <cstring>
#include <new>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace exsim::engine {

// Fixed-size, cache-line aligned array of trivially copyable records,
// allocated once at construction and never resized. The constructor
// zero-fills the whole array, which also pre-faults every page so the hot
// path never takes a first-touch page fault.
template <typename T>
class FixedArray {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                  "FixedArray holds plain records only");

public:
    static constexpr std::size_t kAlignment = 64;

    FixedArray() = default;

    explicit FixedArray(std::size_t element_count) : size_(element_count) {
        if (size_ > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::length_error("arena array byte size overflow");
        if (size_ != 0) {
            data_ = static_cast<T*>(::operator new(size_ * sizeof(T), std::align_val_t{kAlignment}));
            std::memset(static_cast<void*>(data_), 0, size_ * sizeof(T));
        }
    }

    ~FixedArray() { release(); }

    FixedArray(FixedArray&& other) noexcept : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    FixedArray& operator=(FixedArray&& other) noexcept {
        if (this != &other) {
            release();
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    FixedArray(const FixedArray&) = delete;
    FixedArray& operator=(const FixedArray&) = delete;

    [[nodiscard]] T& operator[](std::size_t i) noexcept { return data_[i]; }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return data_[i]; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    void release() noexcept {
        if (data_ != nullptr) {
            ::operator delete(static_cast<void*>(data_), std::align_val_t{kAlignment});
        }
        data_ = nullptr;
        size_ = 0;
    }

    T* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace exsim::engine

#endif
