/**
 * @file ring_buffer.h
 * @brief 无锁环形队列实现
 * 
 * 这是负载均衡器的核心组件之一，用于多核之间的高效数据传递。
 * 
 * 设计目标：
 * 1. 无锁操作：使用 CAS (Compare-And-Swap) 实现线程安全
 * 2. 缓存友好：尽可能避免伪共享 (False Sharing)
 * 3. 高吞吐量：适合高频的生产-消费场景
 * 
 * 实现说明：
 * - 使用 2 的幂次大小，利用位运算替代取模
 * - head/tail 使用 atomic 变量确保可见性
 * - 添加 padding 避免伪共享
 * 
 * 支持两种模式：
 * 1. SPSC (Single Producer Single Consumer): 单生产者单消费者
 * 2. MPMC (Multi Producer Multi Consumer): 多生产者多消费者
 * 
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CORE_RING_BUFFER_H
#define L4LB_CORE_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace l4lb {

/// 缓存行大小（现代 CPU 通常是 64 字节）
constexpr size_t CACHE_LINE_SIZE = 64;

/**
 * @brief 用于避免伪共享的填充结构
 * 
 * 伪共享 (False Sharing) 是多核编程中的性能杀手：
 * 当两个不同核心的变量位于同一缓存行时，一个核心的写入会
 * 导致另一个核心的缓存失效，即使它们访问的是不同的变量。
 */
template<typename T>
struct alignas(CACHE_LINE_SIZE) CacheLineAligned {
    T value;
    
    // 填充字节，确保结构体占用完整的缓存行
    char padding[CACHE_LINE_SIZE - sizeof(T)];
    
    CacheLineAligned() : value{} {}
    explicit CacheLineAligned(const T& v) : value(v) {}
};

/**
 * @brief SPSC 无锁环形队列
 * 
 * Single Producer Single Consumer 队列：
 * - 恰好一个线程调用 push
 * - 恰好一个线程调用 pop
 * 
 * 这是最高效的无锁队列形式，只需要：
 * - 生产者更新 tail
 * - 消费者更新 head
 * 不需要 CAS 操作，只需要内存屏障
 * 
 * @tparam T 元素类型
 * @tparam Size 队列大小（必须是 2 的幂）
 */
template<typename T, size_t Size>
class SPSCRingBuffer {
    // 静态断言：大小必须是 2 的幂
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    static_assert(Size >= 2, "Size must be at least 2");
    
public:
    /**
     * @brief 构造函数
     */
    SPSCRingBuffer() : head_(0), tail_(0) {
        // 初始化缓冲区
        buffer_.resize(Size);
    }
    
    /**
     * @brief 入队操作（生产者调用）
     * 
     * @param item 要入队的元素
     * @return true 入队成功，false 队列已满
     * 
     * 实现说明：
     * 1. 检查队列是否已满
     * 2. 写入数据
     * 3. 使用 release 语义更新 tail（确保写入对消费者可见）
     */
    bool push(const T& item) {
        // 读取当前 tail（写者独占，无需原子操作）
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) & (Size - 1);
        
        // 检查队列是否已满
        // 需要 acquire 语义读取 head，确保看到消费者的最新更新
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // 队列满
        }
        
        // 写入数据
        buffer_[current_tail] = item;
        
        // 更新 tail，使用 release 语义确保写入对消费者可见
        tail_.store(next_tail, std::memory_order_release);
        
        return true;
    }
    
    /**
     * @brief 入队操作（移动语义）
     */
    bool push(T&& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) & (Size - 1);
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        
        buffer_[current_tail] = std::move(item);
        tail_.store(next_tail, std::memory_order_release);
        
        return true;
    }
    
    /**
     * @brief 出队操作（消费者调用）
     * 
     * @param item 出队元素的输出参数
     * @return true 出队成功，false 队列为空
     * 
     * 实现说明：
     * 1. 检查队列是否为空
     * 2. 读取数据
     * 3. 使用 release 语义更新 head（确保读取完成后生产者才能写入该位置）
     */
    bool pop(T& item) {
        // 读取当前 head（读者独占，无需原子操作）
        size_t current_head = head_.load(std::memory_order_relaxed);
        
        // 检查队列是否为空
        // 需要 acquire 语义读取 tail，确保看到生产者的最新更新
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false;  // 队列空
        }
        
        // 读取数据
        item = std::move(buffer_[current_head]);
        
        // 更新 head，使用 release 语义
        size_t next_head = (current_head + 1) & (Size - 1);
        head_.store(next_head, std::memory_order_release);
        
        return true;
    }
    
    /**
     * @brief 查看队首元素但不出队
     */
    bool peek(T& item) const {
        size_t current_head = head_.load(std::memory_order_relaxed);
        
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        
        item = buffer_[current_head];
        return true;
    }
    
    /**
     * @brief 获取当前队列大小
     */
    size_t size() const {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return (tail - head + Size) & (Size - 1);
    }
    
    /**
     * @brief 检查队列是否为空
     */
    bool empty() const {
        return head_.load(std::memory_order_relaxed) == 
               tail_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief 检查队列是否已满
     */
    bool full() const {
        size_t next_tail = (tail_.load(std::memory_order_relaxed) + 1) & (Size - 1);
        return next_tail == head_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief 获取队列容量
     */
    static constexpr size_t capacity() {
        return Size - 1;  // 保留一个位置用于区分空和满
    }
    
private:
    // 使用缓存行对齐避免伪共享
    // head 和 tail 分别被消费者和生产者访问
    CacheLineAligned<std::atomic<size_t>> head_;
    CacheLineAligned<std::atomic<size_t>> tail_;
    
    // 数据缓冲区
    std::vector<T> buffer_;
};

/**
 * @brief MPMC 无锁环形队列
 * 
 * Multi Producer Multi Consumer 队列：
 * - 多个线程可以同时调用 push
 * - 多个线程可以同时调用 pop
 * 
 * 使用 CAS 操作确保线程安全，性能低于 SPSC 但更通用。
 * 
 * 实现采用序列号方案：
 * - 每个槽位有一个序列号
 * - 生产者等待槽位可写
 * - 消费者等待槽位可读
 * 
 * @tparam T 元素类型
 * @tparam Size 队列大小（必须是 2 的幂）
 */
template<typename T, size_t Size>
class MPMCRingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be power of 2");
    static_assert(Size >= 2, "Size must be at least 2");
    
    /**
     * @brief 槽位结构
     * 
     * 每个槽位包含数据和序列号
     */
    struct Slot {
        std::atomic<size_t> sequence;   ///< 序列号
        T data;                          ///< 数据
        
        Slot() : sequence(0) {}
    };
    
public:
    MPMCRingBuffer() : head_(0), tail_(0) {
        buffer_.resize(Size);
        // 初始化序列号
        for (size_t i = 0; i < Size; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
    
    /**
     * @brief 入队操作
     * 
     * @param item 要入队的元素
     * @return true 入队成功，false 队列已满
     * 
     * 实现说明：
     * 使用 CAS 循环获取写入位置，确保多生产者安全
     */
    bool push(const T& item) {
        size_t pos;
        Slot* slot;
        
        while (true) {
            pos = tail_.load(std::memory_order_relaxed);
            slot = &buffer_[pos & (Size - 1)];
            size_t seq = slot->sequence.load(std::memory_order_acquire);
            
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            
            if (diff == 0) {
                // 槽位可写，尝试 CAS 更新 tail
                if (tail_.compare_exchange_weak(pos, pos + 1, 
                                                 std::memory_order_relaxed)) {
                    break;  // 成功获取写入权
                }
            } else if (diff < 0) {
                // 队列满
                return false;
            }
            // else: 被其他生产者抢先，重试
        }
        
        // 写入数据
        slot->data = item;
        
        // 更新序列号，标记槽位可读
        slot->sequence.store(pos + 1, std::memory_order_release);
        
        return true;
    }
    
    /**
     * @brief 出队操作
     * 
     * @param item 出队元素的输出参数
     * @return true 出队成功，false 队列为空
     */
    bool pop(T& item) {
        size_t pos;
        Slot* slot;
        
        while (true) {
            pos = head_.load(std::memory_order_relaxed);
            slot = &buffer_[pos & (Size - 1)];
            size_t seq = slot->sequence.load(std::memory_order_acquire);
            
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            
            if (diff == 0) {
                // 槽位可读，尝试 CAS 更新 head
                if (head_.compare_exchange_weak(pos, pos + 1,
                                                 std::memory_order_relaxed)) {
                    break;  // 成功获取读取权
                }
            } else if (diff < 0) {
                // 队列空
                return false;
            }
            // else: 被其他消费者抢先，重试
        }
        
        // 读取数据
        item = std::move(slot->data);
        
        // 更新序列号，标记槽位可写
        slot->sequence.store(pos + Size, std::memory_order_release);
        
        return true;
    }
    
    /**
     * @brief 获取当前队列大小（近似值）
     */
    size_t size() const {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return tail - head;
    }
    
    /**
     * @brief 检查队列是否为空
     */
    bool empty() const {
        return size() == 0;
    }
    
    /**
     * @brief 获取队列容量
     */
    static constexpr size_t capacity() {
        return Size;
    }
    
private:
    // 缓存行对齐
    CacheLineAligned<std::atomic<size_t>> head_;
    CacheLineAligned<std::atomic<size_t>> tail_;
    
    std::vector<Slot> buffer_;
};

/**
 * @brief 批量操作的包装器
 * 
 * 提供批量 push/pop 操作，减少原子操作次数，提高吞吐量
 * 
 * @tparam RingBuffer 底层环形队列类型
 */
template<typename RingBuffer>
class BatchRingBuffer {
public:
    using ValueType = typename std::remove_reference<
        decltype(std::declval<RingBuffer>().pop(std::declval<typename RingBuffer::value_type&>()),
                 std::declval<typename RingBuffer::value_type>())>::type;
    
    explicit BatchRingBuffer(RingBuffer& rb) : ring_(rb) {}
    
    /**
     * @brief 批量入队
     * 
     * @param items 元素数组
     * @param count 元素数量
     * @return 成功入队的数量
     */
    template<typename T>
    size_t push_batch(const T* items, size_t count) {
        size_t pushed = 0;
        for (size_t i = 0; i < count && ring_.push(items[i]); ++i) {
            ++pushed;
        }
        return pushed;
    }
    
    /**
     * @brief 批量出队
     * 
     * @param items 输出数组
     * @param max_count 最大出队数量
     * @return 成功出队的数量
     */
    template<typename T>
    size_t pop_batch(T* items, size_t max_count) {
        size_t popped = 0;
        for (size_t i = 0; i < max_count && ring_.pop(items[i]); ++i) {
            ++popped;
        }
        return popped;
    }
    
private:
    RingBuffer& ring_;
};

} // namespace l4lb

#endif // L4LB_CORE_RING_BUFFER_H
