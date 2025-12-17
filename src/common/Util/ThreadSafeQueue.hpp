#pragma once

#include <condition_variable>
#include <deque>
#include <optional>
#include <shared_mutex>
#include <vector>

/**
 * Thread-safe producer-consumer queue.
 * Supports blocking and non-blocking pop operations.
 */
template <typename T> class ThreadSafeQueue {
  public:
    ThreadSafeQueue() = default;
    ~ThreadSafeQueue() = default;

    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

    /**
     * Push an item to the queue. Thread-safe.
     */
    void push(T item) {
        {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            _queue.push_back(std::move(item));
        }
        _condVar.notify_one(); // Wake up one waiting thread
    }

    /**
     * Try to pop an item without blocking.
     * Returns std::nullopt if queue is empty.
     */
    std::optional<T> try_pop() {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        if (_queue.empty()) {
            return std::nullopt;
        }
        T item = std::move(_queue.front());
        _queue.pop_front();
        return item;
    }

    /**
     * Bulk dequeue - pop up to maxItems items in a single lock acquisition.
     * Returns the number of items actually popped (may be less than maxItems).
     * This dramatically reduces lock contention when processing many items.
     */
    size_t try_pop_batch(std::vector<T>& outItems, size_t maxItems) {
        std::unique_lock<std::shared_mutex> lock(_mutex);

        const size_t available = _queue.size();
        const size_t toExtract = std::min(available, maxItems);

        if (toExtract == 0) {
            return 0;
        }

        // Reserve space to avoid reallocations
        outItems.reserve(outItems.size() + toExtract);

        // Move items from queue to output vector
        for (size_t i = 0; i < toExtract; ++i) {
            outItems.push_back(std::move(_queue.front()));
            _queue.pop_front();
        }

        return toExtract;
    }

    /**
     * Pop an item, blocking until one is available.
     * Returns false if queue is closed/shutting down.
     */
    bool wait_and_pop(T& outItem) {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _condVar.wait(lock, [this] { return !_queue.empty() || _shutdown; });

        if (_shutdown && _queue.empty()) {
            return false; // Queue is closed and empty
        }

        outItem = std::move(_queue.front());
        _queue.pop_front();
        return true;
    }

    /**
     * Check if queue is empty (snapshot at time of call).
     */
    bool empty() const {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        return _queue.empty();
    }

    /**
     * Get current size (snapshot at time of call).
     */
    size_t size() const {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        return _queue.size();
    }

    /**
     * Signal shutdown to wake up all waiting threads.
     */
    void shutdown() {
        {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            _shutdown = true;
        }
        _condVar.notify_all();
    }

  private:
    mutable std::shared_mutex _mutex;
    std::condition_variable_any _condVar;
    std::deque<T> _queue;
    bool _shutdown = false;
};
