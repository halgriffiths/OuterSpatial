//
// Created by henry on 31/12/2021.
//

#ifndef CPPBAZAARBOT_CONCURRENCY_H
#define CPPBAZAARBOT_CONCURRENCY_H
#include <optional>
#include <thread>
#include <queue>
#include <mutex>

std::int64_t to_unix_timestamp_ms(const std::chrono::system_clock::time_point& time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

template<typename T>
class SafeQueue {
    std::queue<T> queue_;
    mutable std::mutex mutex_;

    // Moved out of public interface to prevent races between this
    // and pop().
    bool empty() const {
        return queue_.empty();
    }

public:
    SafeQueue() = default;
    SafeQueue(const SafeQueue<T> &) = delete ;
    SafeQueue& operator=(const SafeQueue<T> &) = delete ;

    SafeQueue(SafeQueue<T>&& other) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_ = std::move(other.queue_);
    }

    virtual ~SafeQueue() { }

    unsigned long size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return {};
        }
        T tmp = queue_.front();
        queue_.pop();
        return tmp;
    }

    void push(const T &item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
    }
};
#endif//CPPBAZAARBOT_CONCURRENCY_H
