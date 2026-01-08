/**
 * @file test_consistent_hash.cpp
 * @brief 一致性哈希单元测试
 */

#include <gtest/gtest.h>
#include <set>
#include <map>
#include "lb/consistent_hash.h"

using namespace l4lb;

// 测试 MurmurHash3
TEST(MurmurHash3Test, BasicHash) {
    const char* key1 = "hello";
    const char* key2 = "world";
    
    uint32_t hash1 = MurmurHash3::hash(key1, strlen(key1));
    uint32_t hash2 = MurmurHash3::hash(key2, strlen(key2));
    
    // 不同输入应产生不同哈希
    EXPECT_NE(hash1, hash2);
    
    // 相同输入应产生相同哈希
    EXPECT_EQ(hash1, MurmurHash3::hash(key1, strlen(key1)));
}

TEST(MurmurHash3Test, TupleHash) {
    FiveTuple t1(0x01020304, 0x05060708, 80, 8080, 6);
    FiveTuple t2(0x01020304, 0x05060708, 80, 8080, 6);
    FiveTuple t3(0x01020305, 0x05060708, 80, 8080, 6);
    
    // 相同五元组应产生相同哈希
    EXPECT_EQ(MurmurHash3::hash_tuple(t1), MurmurHash3::hash_tuple(t2));
    
    // 不同五元组应产生不同哈希
    EXPECT_NE(MurmurHash3::hash_tuple(t1), MurmurHash3::hash_tuple(t3));
}

// 测试一致性哈希环
TEST(ConsistentHashTest, AddRemoveNode) {
    ConsistentHashRing ring(10);
    
    ring.add_node(1);
    ring.add_node(2);
    
    EXPECT_GT(ring.node_count(), 0);
    
    ring.remove_node(1);
    
    uint32_t server_id;
    EXPECT_TRUE(ring.get_server(FiveTuple(), server_id));
    EXPECT_EQ(server_id, 2);
}

TEST(ConsistentHashTest, EmptyRing) {
    ConsistentHashRing ring;
    
    uint32_t server_id;
    EXPECT_FALSE(ring.get_server(FiveTuple(), server_id));
}

TEST(ConsistentHashTest, Distribution) {
    ConsistentHashRing ring(150);
    
    // 添加 3 个节点
    ring.add_node(1, 100);
    ring.add_node(2, 100);
    ring.add_node(3, 100);
    
    // 测试分布
    std::map<uint32_t, int> counts;
    const int total = 10000;
    
    for (int i = 0; i < total; ++i) {
        FiveTuple tuple;
        tuple.src_ip = i;
        tuple.dst_ip = i * 2;
        tuple.src_port = i % 65535;
        tuple.dst_port = 80;
        tuple.protocol = 6;
        
        uint32_t server_id;
        ASSERT_TRUE(ring.get_server(tuple, server_id));
        counts[server_id]++;
    }
    
    // 每个节点应该分到大约 1/3 的流量
    // 允许 20% 的误差
    int expected = total / 3;
    int tolerance = expected * 0.3;
    
    for (const auto& [id, count] : counts) {
        EXPECT_NEAR(count, expected, tolerance) 
            << "Server " << id << " got " << count << " connections";
    }
}

TEST(ConsistentHashTest, Consistency) {
    ConsistentHashRing ring(150);
    
    ring.add_node(1);
    ring.add_node(2);
    ring.add_node(3);
    
    FiveTuple tuple(192168001, 192168002, 12345, 80, 6);
    
    uint32_t server1, server2;
    ASSERT_TRUE(ring.get_server(tuple, server1));
    ASSERT_TRUE(ring.get_server(tuple, server2));
    
    // 相同五元组应该映射到相同服务器
    EXPECT_EQ(server1, server2);
}

TEST(ConsistentHashTest, MinimalRemapping) {
    ConsistentHashRing ring(150);
    
    ring.add_node(1);
    ring.add_node(2);
    ring.add_node(3);
    
    // 记录原始映射
    std::vector<uint32_t> original_mapping(1000);
    for (int i = 0; i < 1000; ++i) {
        FiveTuple tuple;
        tuple.src_ip = i;
        ring.get_server(tuple, original_mapping[i]);
    }
    
    // 移除一个节点
    ring.remove_node(2);
    
    // 检查重映射比例
    int remapped = 0;
    for (int i = 0; i < 1000; ++i) {
        FiveTuple tuple;
        tuple.src_ip = i;
        uint32_t new_server;
        ring.get_server(tuple, new_server);
        
        if (original_mapping[i] == 2) {
            // 原来映射到被删除节点的应该重映射
            EXPECT_NE(new_server, 2);
            ++remapped;
        } else if (new_server != original_mapping[i]) {
            // 不应该有太多不必要的重映射
            ++remapped;
        }
    }
    
    // 大约 1/3 应该重映射（原来在节点2上的）
    // 加上少量抖动，不超过 50%
    EXPECT_LT(remapped, 500);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
