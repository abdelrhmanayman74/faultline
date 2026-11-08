#include "faultline/thread_pool.hpp"

#include <stdexcept>
#include <utility>

namespace faultline {

ThreadPool::ThreadPool(std::size_t num_threads, std::size_t queue_capacity)
    : queue_(queue_capacity) {
    if (num_threads == 0) {
        throw std::invalid_argument("ThreadPool needs at least one thread");
    }
    workers_.reserve(num_threads);
    try {
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        // Starting a thread can fail (std::system_error). The destructor will
        // not run for a half-built object, but workers_ is destroyed and each
        // jthread joins; close the queue first so the started workers exit.
        queue_.close();
        throw;
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

std::future<void> ThreadPool::submit(std::function<void()> job) {
    Task task(std::move(job));
    std::future<void> result = task.get_future();
    if (!queue_.push(std::move(task))) {
        throw std::runtime_error("ThreadPool::submit after shutdown");
    }
    return result;
}

std::optional<std::future<void>> ThreadPool::try_submit(std::function<void()> job) {
    Task task(std::move(job));
    std::future<void> result = task.get_future();
    if (queue_.try_push(std::move(task)) != PushResult::Ok) {
        return std::nullopt;  // the task is destroyed unrun; its future is dropped too
    }
    return result;
}

void ThreadPool::shutdown() {
    std::call_once(shutdown_once_, [this] {
        queue_.close();  // workers drain the queue, then pop() returns nullopt
        for (std::jthread& worker : workers_) {
            worker.join();
        }
    });
}

void ThreadPool::worker_loop() {
    // pop() blocks while the queue is empty and returns nullopt only once it
    // is closed and drained: that is the only way out of the loop.
    while (std::optional<Task> task = queue_.pop()) {
        (*task)();  // never throws the job's exception: packaged_task stores it
    }
}

}  // namespace faultline
