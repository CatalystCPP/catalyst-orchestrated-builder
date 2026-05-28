#pragma once

#include <atomic>
#include <bit>
#include <memory>
#include <cstddef>
#include <cstdint>

namespace catalyst {

/**
 * @brief A lock-free, bounded Multiple-Producer Single-Consumer (MPSC) queue.
 * Based on Dmitry Vyukov's bounded MPMC queue algorithm, but optimized for a single consumer.
 */
template <typename T>
class LockFreeMPSCQueue {
public:
    explicit LockFreeMPSCQueue(size_t capacity) {
        // Capacity must be a power of two for fast bitmask operations
        size_t cap = std::bit_ceil(capacity);
        buffer_ = std::make_unique<Cell[]>(cap);
        buffer_mask_ = cap - 1;
        for (size_t i = 0; i < cap; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_ = 0; // Only modified by consumer thread
    }

    /**
     * @brief Enqueues an item into the queue. Safe to call concurrently by multiple producers.
     * @param data The item to enqueue.
     * @return true if successful, false if the queue is full.
     */
    bool enqueue(T const& data) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        while (true) {
            cell = &buffer_[pos & buffer_mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // Queue is full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = data;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Dequeues an item from the queue. ONLY safe to call by a SINGLE consumer.
     * @param data Reference where the dequeued item will be stored.
     * @return true if successful, false if the queue is empty.
     */
    bool dequeue(T& data) {
        Cell* cell;
        size_t pos = dequeue_pos_;
        cell = &buffer_[pos & buffer_mask_];
        size_t seq = cell->sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

        if (diff == 0) {
            data = std::move(cell->data);
            cell->sequence.store(pos + buffer_mask_ + 1, std::memory_order_release);
            dequeue_pos_ = pos + 1;
            return true;
        }

        return false; // Queue is empty
    }

private:
    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    static constexpr size_t CACHELINE_SIZE = 64;

    alignas(CACHELINE_SIZE) std::unique_ptr<Cell[]> buffer_;
    size_t buffer_mask_;
    alignas(CACHELINE_SIZE) std::atomic<size_t> enqueue_pos_;
    // No atomic needed since it's only modified/read by the single consumer thread
    alignas(CACHELINE_SIZE) size_t dequeue_pos_;
};

} // namespace catalyst
