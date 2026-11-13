#ifndef EXSIM_UTIL_OBJECT_POOL_HPP
#define EXSIM_UTIL_OBJECT_POOL_HPP

#include "compiler.hpp"
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace exsim::util {

// Free-list pool. reserve() allocates exactly the requested node count in one
// contiguous block; when growth is allowed, an empty free list adds a further
// block of BlockSize nodes. Live objects are released with their blocks, so T
// must not need its destructor run.
template <typename T, std::size_t BlockSize = 4096>
class ObjectPool {
    static_assert(BlockSize > 0, "pool growth block must contain a node");
    static_assert(std::is_trivially_destructible_v<T>, "pool blocks are freed without running destructors");

public:
    ObjectPool() = default;
    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    // Ensures at least `object_count` nodes exist in total (live or free).
    void reserve(std::size_t object_count) {
        if (object_count > node_count_) {
            allocate_block(object_count - node_count_);
        }
    }

    void set_allow_growth(bool allow_growth) noexcept {
        allow_growth_ = allow_growth;
    }

    // Adds a block if no node is free, so the next create() cannot allocate.
    void ensure_free_node() {
        if (!free_list_) {
            allocate_block(BlockSize);
        }
    }

    // Returns nullptr when the pool is empty and growth is disabled.
    template <typename... Args>
    T* create(Args&&... args) {
        if (EXSIM_UNLIKELY(!free_list_)) {
            if (EXSIM_UNLIKELY(!allow_growth_)) {
                return nullptr;
            }
            allocate_block(BlockSize);
        }

        Node* node = free_list_;
        free_list_ = free_list_->next;
        try {
            return ::new (static_cast<void*>(node->storage)) T(std::forward<Args>(args)...);
        } catch (...) {
            node->next = free_list_;
            free_list_ = node;
            throw;
        }
    }

    void destroy(T* object) noexcept {
        if (!object) {
            return;
        }

        object->~T();
        Node* node = reinterpret_cast<Node*>(object);
        node->next = free_list_;
        free_list_ = node;
    }

private:
    union Node {
        alignas(T) unsigned char storage[sizeof(T)];
        Node* next;
    };

    void allocate_block(std::size_t node_count) {
        // Owned before blocks_ can throw while growing, so a failure leaks nothing.
        std::unique_ptr<Node[]> block(new Node[node_count]);
        blocks_.push_back(std::move(block));
        Node* nodes = blocks_.back().get();

        for (std::size_t i = 0; i + 1 < node_count; ++i) {
            nodes[i].next = &nodes[i + 1];
        }
        nodes[node_count - 1].next = free_list_;
        free_list_ = nodes;
        node_count_ += node_count;
    }

    std::vector<std::unique_ptr<Node[]>> blocks_;
    Node* free_list_ = nullptr;
    std::size_t node_count_ = 0;
    bool allow_growth_ = true;
};

}  // namespace exsim::util

#endif
