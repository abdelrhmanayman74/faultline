#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "faultline/bounded_queue.hpp"

namespace faultline {

// Fixed number of worker threads that take jobs from one BoundedQueue.
//
// Results and errors: each job is wrapped in a std::packaged_task, and submit()
// returns the matching std::future<void>. If the job throws, packaged_task
// stores the exception in the future instead of letting it escape, so the
// worker survives and future.get() rethrows it in the thread that asked for
// the result. A job that produces data writes it through what it captured
// (for example its own slice of an output array). If nobody calls get(), an
// exception is silently dropped with the future.
//
// Cancellation is cooperative and owned by the caller: create a
// std::stop_source per job (or per group of jobs), capture its token in the
// job, and have the job check token.stop_requested() at safe points. A thread
// cannot be killed safely from outside (it may hold a lock or be half way
// through writing data), so the job has to stop itself.
//
// Never wait on a future of this pool from inside one of its jobs: if every
// worker does that, no worker is left to run the jobs being waited for.
class ThreadPool {
public:
    // Starts `num_threads` workers (at least 1). At most `queue_capacity` jobs
    // (at least 1) wait in the queue; running jobs do not count.
    ThreadPool(std::size_t num_threads, std::size_t queue_capacity);
    ~ThreadPool();  // calls shutdown()

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Blocks while the queue is full. Throws std::runtime_error after shutdown.
    std::future<void> submit(std::function<void()> job);

    // Never blocks. Returns std::nullopt if the queue is full or the pool is
    // shut down; the caller can then reject the request (backpressure).
    std::optional<std::future<void>> try_submit(std::function<void()> job);

    // Stops accepting jobs, lets the workers finish every job already queued,
    // then joins them. Safe to call more than once and from several threads,
    // but not from inside a job (a worker cannot join itself). It does not
    // interrupt running jobs: request a stop on their tokens first if they are
    // long.
    void shutdown();

    std::size_t thread_count() const { return workers_.size(); }
    std::size_t queued() const { return queue_.size(); }

private:
    using Task = std::packaged_task<void()>;  // move-only: the queue moves it, never copies

    void worker_loop();

    BoundedQueue<Task> queue_;
    // std::jthread joins in its destructor, so no path (not even an exception
    // in the constructor) can leave a joinable std::thread behind, which would
    // call std::terminate.
    std::vector<std::jthread> workers_;
    std::once_flag shutdown_once_;  // makes shutdown() run its body exactly once
};

}  // namespace faultline
