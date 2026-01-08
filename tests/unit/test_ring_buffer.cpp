/**
 * @file test_ring_buffer.cpp
 * @brief 无锁环形队列单元测试
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include "core/ring_buffer.h"

using namespace l4lb;

// 测试 SPSC 队列基本操作
TEST(SPSCRingBufferTest, BasicOperations) {
    SPSCRingBuffer<int, 8> rb;
    
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0);
    
    // Push
    EXPECT_TRUE(rb.push(1));
    EXPECT_TRUE(rb.push(2));
    EXPECT_TRUE(rb.push(3));
    
    EXPECT_EQ(rb.size(), 3);
    EXPECT_FALSE(rb.empty());
    
    // Pop
    int val;
    EXPECT_TRUE(rb.pop(val));
    EXPECT_EQ(val, 1);
    
    EXPECT_TRUE(rb.pop(val));
    EXPECT_EQ(val, 2);
    
    EXPECT_TRUE(rb.pop(val));
    EXPECT_EQ(val, 3);
    
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.pop(val));
}

TEST(SPSCRingBufferTest, FullBuffer) {
    SPSCRingBuffer<int, 4> rb;  // 容量为 3（保留1个位置）
    
    EXPECT_TRUE(rb.push(1));
    EXPECT_TRUE(rb.push(2));
    EXPECT_TRUE(rb.push(3));
    EXPECT_FALSE(rb.push(4));  // 队列满
    
    EXPECT_TRUE(rb.full());
}

TEST(SPSCRingBufferTest, Wraparound) {
    SPSCRingBuffer<int, 4> rb;
    
    // 填满并清空多次，测试环绕
    for (int round = 0; round < 10; ++round) {
        EXPECT_TRUE(rb.push(round * 3 + 1));
        EXPECT_TRUE(rb.push(round * 3 + 2));
        EXPECT_TRUE(rb.push(round * 3 + 3));
        
        int val;
        EXPECT_TRUE(rb.pop(val));
        EXPECT_EQ(val, round * 3 + 1);
        EXPECT_TRUE(rb.pop(val));
        EXPECT_EQ(val, round * 3 + 2);
        EXPECT_TRUE(rb.pop(val));
        EXPECT_EQ(val, round * 3 + 3);
    }
}

TEST(SPSCRingBufferTest, ConcurrentPushPop) {
    SPSCRingBuffer<int, 1024> rb;
    const int count = 100000;
    std::atomic<int> sum_push{0};
    std::atomic<int> sum_pop{0};
    
    std::thread producer([&]() {
        for (int i = 0; i < count; ++i) {
            while (!rb.push(i)) {
                std::this_thread::yield();
            }
            sum_push += i;
        }
    });
    
    std::thread consumer([&]() {
        int val;
        int received = 0;
        while (received < count) {
            if (rb.pop(val)) {
                sum_pop += val;
                ++received;
            }
        }
    });
    
    producer.join();
    consumer.join();
    
    EXPECT_EQ(sum_push.load(), sum_pop.load());
}

// 测试 MPMC 队列
TEST(MPMCRingBufferTest, BasicOperations) {
    MPMCRingBuffer<int, 8> rb;
    
    EXPECT_TRUE(rb.empty());
    
    EXPECT_TRUE(rb.push(1));
    EXPECT_TRUE(rb.push(2));
    
    int val;
    EXPECT_TRUE(rb.pop(val));
    EXPECT_EQ(val, 1);
    EXPECT_TRUE(rb.pop(val));
    EXPECT_EQ(val, 2);
}

TEST(MPMCRingBufferTest, MultipleProducers) {
    MPMCRingBuffer<int, 1024> rb;
    const int per_thread = 10000;
    const int num_producers = 4;
    std::atomic<int> produced{0};
    
    std::vector<std::thread> producers;
    for (int t = 0; t < num_producers; ++t) {
        producers.emplace_back([&, t]() {
            for (int i = 0; i < per_thread; ++i) {
                while (!rb.push(t * per_thread + i)) {
                    std::this_thread::yield();
                }
                ++produced;
            }
        });
    }
    
    std::vector<int> consumed;
    consumed.reserve(num_producers * per_thread);
    
    std::thread consumer([&]() {
        int val;
        while (produced < num_producers * per_thread || !rb.empty()) {
            if (rb.pop(val)) {
                consumed.push_back(val);
            }
        }
    });
    
    for (auto& p : producers) p.join();
    consumer.join();
    
    EXPECT_EQ(consumed.size(), num_producers * per_thread);
}

TEST(MPMCRingBufferTest, MultipleConsumers) {
    MPMCRingBuffer<int, 1024> rb;
    const int total = 40000;
    std::atomic<int> consumed{0};
    std::atomic<bool> done{false};
    
    // 生产者
    std::thread producer([&]() {
        for (int i = 0; i < total; ++i) {
            while (!rb.push(i)) {
                std::this_thread::yield();
            }
        }
        done = true;
    });
    
    // 多个消费者
    const int num_consumers = 4;
    std::vector<std::thread> consumers;
    for (int t = 0; t < num_consumers; ++t) {
        consumers.emplace_back([&]() {
            int val;
            while (!done || !rb.empty()) {
                if (rb.pop(val)) {
                    ++consumed;
                }
            }
        });
    }
    
    producer.join();
    for (auto& c : consumers) c.join();
    
    EXPECT_EQ(consumed.load(), total);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
