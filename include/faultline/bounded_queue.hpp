#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace faultline {

enum class PushResult {
    Ok,      // the item is in the queue
    Full,    // try_push only: no free slot right now (backpressure)
    Closed,  // close() was called; the item was not added
};

// Fixed-capacity FIFO queue shared by producer and consumer threads.
//
// One std::mutex protects every member below it. Two condition variables let
// a thread sleep until the state it needs:
//   not_full_  : producers wait here while the queue is full;
//   not_empty_ : consumers wait here while the queue is empty.
// Why two and not one? With a single condition variable, producers and
// consumers sleep on the same list. notify_one() after a push could then wake
// another producer instead of a consumer; that producer sees "still full" and
// sleeps again, the consumer never hears about the item, and the program can
// stall. With one variable per condition, a notify always reaches a thread
// that can use it, so notify_one() is enough and nobody is woken for nothing.
//
// Every wait uses the predicate form, wait(lock, predicate). It is the same as
//     while (!predicate()) cv.wait(lock);
// The loop is required: a thread can wake without a notify (a "spurious
// wakeup", allowed by the standard), and even after a real notify another
// thread may get the mutex first and take the slot or the item. So the
// condition is always re-checked under the lock before going on.
//
// Notify is called after the lock is released. Correctness does not depend on
// that: the state change happens under the lock and the waiter re-checks it
// under the lock, so a wakeup cannot be lost either way. Notifying after
// unlocking just avoids waking a thread that would immediately block again on
// the mutex we still hold. (Like any object, the queue must outlive every call
// on it; the owner must not destroy it while a push/pop/close is running.)
//
// close() is the shutdown signal: after it, push fails, and pop drains what is
// left and then returns std::nullopt. Consumers use that nullopt as "exit".
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedQueue capacity must be at least 1");
        }
    }

    // A mutex can be neither copied nor moved, and threads hold references to
    // the queue, so the queue itself is pinned in memory.
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Blocks while the queue is full. Returns false (and drops `item`) if the
    // queue is closed, also when close() happens while waiting.
    bool push(T item) {
        {
            std::unique_lock lock(mutex_);  // unique_lock: wait() must unlock and relock it
            not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
            if (closed_) {
                return false;
            }
            items_.push_back(std::move(item));
        }
        not_empty_.notify_one();  // one new item: one consumer can use it
        return true;
    }

    // Never blocks: the caller decides what to do when the queue is full (a
    // server answers "busy" instead of letting the client wait). On Full or
    // Closed the item is not added and is destroyed with the parameter.
    PushResult try_push(T item) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return PushResult::Closed;
            }
            if (items_.size() >= capacity_) {
                return PushResult::Full;
            }
            items_.push_back(std::move(item));
        }
        not_empty_.notify_one();
        return PushResult::Ok;
    }

    // Blocks while the queue is empty and open. Returns the oldest item, or
    // std::nullopt once the queue is closed AND empty: items pushed before
    // close() are still delivered.
    std::optional<T> pop() {
        std::optional<T> item;
        {
            std::unique_lock lock(mutex_);
            not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
            if (items_.empty()) {
                return std::nullopt;  // closed and drained
            }
            item.emplace(std::move(items_.front()));
            items_.pop_front();
        }
        not_full_.notify_one();  // one slot freed: one producer can use it
        return item;
    }

    // Wakes every waiting thread. Blocked producers return false, blocked
    // consumers drain the remaining items and then get std::nullopt.
    // Idempotent.
    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_full_.notify_all();  // all waiters must re-check: closed_ concerns every one
        not_empty_.notify_all();
    }

    // A snapshot: other threads may change it right after it is returned.
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    std::size_t capacity() const { return capacity_; }  // const after construction: no lock

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;  // mutable: size() is const but must lock
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace faultline
