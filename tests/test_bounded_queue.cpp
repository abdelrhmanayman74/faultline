#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "faultline/bounded_queue.hpp"

namespace faultline {
namespace {

using namespace std::chrono_literals;

// How the blocking tests avoid sleep races: a helper thread does the blocking
// call and reports through a future. "Still blocked" is checked with a short
// wait_for that must time out; a slow machine can only make that check pass
// more easily, never fail wrongly. "Woken" is checked with a long wait_for
// that returns as soon as the call finishes, so it costs nothing when it works.
constexpr auto kStillBlocked = 50ms;
constexpr auto kMustFinish = 10s;

TEST(BoundedQueue, FifoOrder) {
    BoundedQueue<int> queue(8);
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(queue.push(i));
    }
    EXPECT_EQ(queue.size(), 5U);
    for (int i = 1; i <= 5; ++i) {
        EXPECT_EQ(queue.pop(), i);
    }
    EXPECT_EQ(queue.size(), 0U);
}

TEST(BoundedQueue, ZeroCapacityIsRejected) {
    EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(BoundedQueue, TryPushFailsWhenFull) {
    BoundedQueue<int> queue(2);
    EXPECT_EQ(queue.try_push(1), PushResult::Ok);
    EXPECT_EQ(queue.try_push(2), PushResult::Ok);
    EXPECT_EQ(queue.try_push(3), PushResult::Full);
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.try_push(3), PushResult::Ok);
    EXPECT_EQ(queue.pop(), 2);
    EXPECT_EQ(queue.pop(), 3);
}

TEST(BoundedQueue, PushBlocksWhileFull) {
    BoundedQueue<int> queue(1);
    ASSERT_TRUE(queue.push(1));
    std::future<bool> pushed = std::async(std::launch::async, [&queue] { return queue.push(2); });
    EXPECT_EQ(pushed.wait_for(kStillBlocked), std::future_status::timeout);

    EXPECT_EQ(queue.pop(), 1);  // frees the slot
    ASSERT_EQ(pushed.wait_for(kMustFinish), std::future_status::ready);
    EXPECT_TRUE(pushed.get());
    EXPECT_EQ(queue.pop(), 2);
}

TEST(BoundedQueue, CloseWakesBlockedPop) {
    BoundedQueue<int> queue(1);
    std::future<std::optional<int>> popped =
        std::async(std::launch::async, [&queue] { return queue.pop(); });
    EXPECT_EQ(popped.wait_for(kStillBlocked), std::future_status::timeout);

    queue.close();
    ASSERT_EQ(popped.wait_for(kMustFinish), std::future_status::ready);
    EXPECT_EQ(popped.get(), std::nullopt);
}

TEST(BoundedQueue, CloseWakesBlockedPushAndKeepsQueuedItems) {
    BoundedQueue<int> queue(1);
    ASSERT_TRUE(queue.push(1));
    std::future<bool> pushed = std::async(std::launch::async, [&queue] { return queue.push(2); });
    EXPECT_EQ(pushed.wait_for(kStillBlocked), std::future_status::timeout);

    queue.close();
    ASSERT_EQ(pushed.wait_for(kMustFinish), std::future_status::ready);
    EXPECT_FALSE(pushed.get());
    EXPECT_EQ(queue.pop(), 1);  // pushed before close(): still delivered
    EXPECT_EQ(queue.pop(), std::nullopt);
}

TEST(BoundedQueue, PushAfterCloseFails) {
    BoundedQueue<int> queue(4);
    queue.close();
    queue.close();  // idempotent
    EXPECT_FALSE(queue.push(1));
    EXPECT_EQ(queue.try_push(1), PushResult::Closed);
    EXPECT_EQ(queue.size(), 0U);
}

// 4 producers and 4 consumers through a queue of 16, so both sides block
// often. Every item must arrive exactly once, and the items of one producer
// must reach any one consumer in the order they were pushed (FIFO).
TEST(BoundedQueue, ManyProducersManyConsumers) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 25'000;  // 100'000 items in total
    BoundedQueue<int> queue(16);

    std::vector<std::future<std::vector<int>>> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.push_back(std::async(std::launch::async, [&queue] {
            std::vector<int> received;
            std::vector<int> last(kProducers, -1);  // last index seen from each producer
            while (std::optional<int> item = queue.pop()) {
                const int producer = *item / kPerProducer;
                const int index = *item % kPerProducer;
                EXPECT_GT(index, last[producer]);
                last[producer] = index;
                received.push_back(*item);
            }
            return received;
        }));
    }
    {
        std::vector<std::jthread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&queue, p] {
                for (int i = 0; i < kPerProducer; ++i) {
                    EXPECT_TRUE(queue.push(p * kPerProducer + i));
                }
            });
        }
    }  // jthreads join here: every item has been pushed
    queue.close();

    std::vector<int> all;
    for (std::future<std::vector<int>>& consumer : consumers) {
        const std::vector<int> received = consumer.get();
        all.insert(all.end(), received.begin(), received.end());
    }
    ASSERT_EQ(all.size(), static_cast<std::size_t>(kProducers * kPerProducer));
    std::sort(all.begin(), all.end());
    for (int i = 0; i < kProducers * kPerProducer; ++i) {
        ASSERT_EQ(all[static_cast<std::size_t>(i)], i);
    }
}

}  // namespace
}  // namespace faultline
