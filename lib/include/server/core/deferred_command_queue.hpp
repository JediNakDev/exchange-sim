#ifndef EXSIM_SERVER_CORE_DEFERRED_COMMAND_QUEUE_HPP
#define EXSIM_SERVER_CORE_DEFERRED_COMMAND_QUEUE_HPP

// Fixed-capacity FIFO of commands submitted while a batch is being
// dispatched (for example the cancel-on-disconnect MassCancel of a
// connection the server drops from inside a gateway callback). The core
// applies them in order once the current batch has reached every consumer,
// so a nested submit never overwrites the batch being dispatched.

#include "command.hpp"

#include <cstddef>
#include <memory>

namespace exsim::server {

class DeferredCommandQueue {
public:
    explicit DeferredCommandQueue(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity), slots_(std::make_unique<Command[]>(capacity_)) {}

    [[nodiscard]] bool push(const Command& command) noexcept {
        if (size_ == capacity_) {
            return false;
        }
        slots_[(head_ + size_) % capacity_] = command;
        ++size_;
        return true;
    }

    [[nodiscard]] bool pop(Command& command) noexcept {
        if (size_ == 0) {
            return false;
        }
        command = slots_[head_];
        head_ = (head_ + 1) % capacity_;
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_;
    std::unique_ptr<Command[]> slots_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

}  // namespace exsim::server

#endif
