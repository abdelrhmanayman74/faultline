#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

#include "faultline/thread_pool.hpp"

namespace faultline {
namespace {

using namespace std::chrono_literals;

constexpr auto kStillRunning = 50ms;  // see test_bounded_queue.cpp for the idea
constexpr auto kMustFinish = 10s;

TEST(ThreadPool, RunsAllTasks) {
    ThreadPool pool(4, 16);  // 1000 jobs through 16 slots: submit() blocks often
    std::atomic<int> counter{0};
    std::vector<std::future<void>> done;
    for (int i = 0; i < 1000; ++i) {
        done.push_back(pool.submit([&counter] { counter.fetch_add(1); }));
    }
    for (std::future<void>& job : done) {
        job.get();
    }
    EXPECT_EQ(counter.load(), 1000);
    EXPECT_EQ(pool.thread_count(), 4U);
}

TEST(ThreadPool, ZeroThreadsIsRejected) {
    EXPECT_THROW(ThreadPool(0, 1), std::invalid_argument);
}

// One worker only: if the exception had killed it, the second job never runs.
TEST(ThreadPool, ExceptionGoesToFutureAndWorkerSurvives) {
    ThreadPool pool(1, 4);
    std::future<void> failing = pool.submit([] { throw std::runtime_error("job failed"); });
    EXPECT_THROW(failing.get(), std::runtime_error);

    bool ran = false;  // written by the worker, read here after get(): get() synchronizes
    pool.submit([&ran] { ran = true; }).get();
    EXPECT_TRUE(ran);
}

TEST(ThreadPool, TrySubmitFailsWhenQueueFull) {
    ThreadPool pool(1, 1);
    std::promise<void> started;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    // Occupy the only worker, and wait until it really runs (queue empty again).
    std::future<void> blocker = pool.submit([&started, released] {
        started.set_value();
        released.wait();
    });
    started.get_future().wait();

    std::optional<std::future<void>> queued = pool.try_submit([] {});
    ASSERT_TRUE(queued.has_value());                 // fills the single slot
    EXPECT_FALSE(pool.try_submit([] {}).has_value());  // full: rejected, not blocked
    EXPECT_EQ(pool.queued(), 1U);

    release.set_value();
    blocker.get();
    queued->get();
    std::optional<std::future<void>> again = pool.try_submit([] {});
    ASSERT_TRUE(again.has_value());
    again->get();
}

TEST(ThreadPool, ShutdownRunsQueuedJobsAndIsIdempotent) {
    ThreadPool pool(1, 10);
    std::atomic<int> counter{0};
    for (int i = 0; i < 5; ++i) {
        pool.submit([&counter] { counter.fetch_add(1); });  // futures dropped on purpose
    }
    {
        // Two threads at once: call_once lets one join, the other waits for it.
        std::jthread a([&pool] { pool.shutdown(); });
        std::jthread b([&pool] { pool.shutdown(); });
    }
    pool.shutdown();
    EXPECT_EQ(counter.load(), 5);  // queued jobs were drained, not discarded
    EXPECT_THROW(pool.submit([] {}), std::runtime_error);
    EXPECT_FALSE(pool.try_submit([] {}).has_value());
}  // the destructor calls shutdown() once more

TEST(ThreadPool, StopTokenEndsLongJob) {
    ThreadPool pool(2, 4);
    std::stop_source cancel;
    std::promise<void> started;
    std::future<void> job = pool.submit([token = cancel.get_token(), &started] {
        started.set_value();
        while (!token.stop_requested()) {
            std::this_thread::yield();  // stands for one unit of real work
        }
    });
    started.get_future().wait();
    EXPECT_EQ(job.wait_for(kStillRunning), std::future_status::timeout);

    cancel.request_stop();
    EXPECT_EQ(job.wait_for(kMustFinish), std::future_status::ready);
    job.get();
}

}  // namespace
}  // namespace faultline
