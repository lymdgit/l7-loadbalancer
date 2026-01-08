/**
 * @file test_protocol.cpp
 * @brief 协议解析单元测试
 */

#include <gtest/gtest.h>
#include <cstring>
#include "protocol/ethernet.h"
#include "protocol/ip.h"
#include "protocol/arp.h"
#include "protocol/icmp.h"
#include "common/types.h"

using namespace l4lb;

// 测试以太网帧
TEST(EthernetTest, HeaderParse) {
    uint8_t packet[64] = {0};
    
    // 构建以太网帧
    auto* eth = reinterpret_cast<EthernetHeader*>(packet);
    uint8_t dst_mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t src_mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    memcpy(eth->dst_mac, dst_mac, 6);
    memcpy(eth->src_mac, src_mac, 6);
    eth->set_ether_type(static_cast<uint16_t>(EtherType::IPv4));
    
    // 验证解析
    auto* parsed = Ethernet::parse(packet, sizeof(packet));
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(parsed->get_ether_type(), static_cast<uint16_t>(EtherType::IPv4));
    EXPECT_TRUE(parsed->is_ipv4());
}

TEST(EthernetTest, BroadcastDetection) {
    EthernetHeader eth;
    memset(eth.dst_mac, 0xFF, 6);
    
    EXPECT_TRUE(eth.is_broadcast());
    
    eth.dst_mac[0] = 0x00;
    EXPECT_FALSE(eth.is_broadcast());
}

// 测试 IP 解析
TEST(IPv4Test, HeaderParse) {
    uint8_t packet[64] = {0};
    
    auto* eth = reinterpret_cast<EthernetHeader*>(packet);
    eth->set_ether_type(static_cast<uint16_t>(EtherType::IPv4));
    
    auto* ip = reinterpret_cast<IPv4Header*>(packet + 14);
    ip->version_ihl = 0x45;  // version=4, ihl=5
    ip->total_length = htons(40);
    ip->ttl = 64;
    ip->protocol = 6;  // TCP
    ip->src_ip = 0x01020304;
    ip->dst_ip = 0x05060708;
    
    // 解析
    PacketMeta meta;
    ASSERT_TRUE(ProtocolParser::parse(packet, 64, meta));
    
    EXPECT_EQ(meta.ether_type, static_cast<uint16_t>(EtherType::IPv4));
    EXPECT_EQ(meta.src_ip, 0x01020304);
    EXPECT_EQ(meta.dst_ip, 0x05060708);
    EXPECT_EQ(meta.ip_protocol, 6);
    EXPECT_EQ(meta.ip_ttl, 64);
}

TEST(IPv4Test, TcpParsing) {
    uint8_t packet[64] = {0};
    
    // 以太网头
    auto* eth = reinterpret_cast<EthernetHeader*>(packet);
    eth->set_ether_type(static_cast<uint16_t>(EtherType::IPv4));
    
    // IP 头
    auto* ip = reinterpret_cast<IPv4Header*>(packet + 14);
    ip->version_ihl = 0x45;
    ip->total_length = htons(40);
    ip->protocol = 6;  // TCP
    ip->src_ip = 0x01020304;
    ip->dst_ip = 0x05060708;
    
    // TCP 头
    auto* tcp = reinterpret_cast<TcpHeader*>(packet + 14 + 20);
    tcp->src_port = htons(12345);
    tcp->dst_port = htons(80);
    tcp->data_offset = 0x50;  // 5 * 4 = 20 bytes
    
    // 解析
    PacketMeta meta;
    ASSERT_TRUE(ProtocolParser::parse(packet, 64, meta));
    
    EXPECT_EQ(meta.src_port, htons(12345));
    EXPECT_EQ(meta.dst_port, htons(80));
}

// 测试 IP 校验和
TEST(ChecksumTest, IPChecksum) {
    uint8_t ip_header[20] = {
        0x45, 0x00, 0x00, 0x28, 0x00, 0x01, 0x00, 0x00,
        0x40, 0x06, 0x00, 0x00, 0xC0, 0xA8, 0x01, 0x01,
        0xC0, 0xA8, 0x01, 0x02
    };
    
    uint16_t checksum = IpChecksum::calculate(ip_header, 20);
    EXPECT_NE(checksum, 0);
}

// 测试 ICMP 校验和
TEST(IcmpTest, Checksum) {
    uint8_t icmp_data[8] = {
        0x08, 0x00, 0x00, 0x00,  // type, code, checksum
        0x00, 0x01, 0x00, 0x01   // id, seq
    };
    
    uint16_t checksum = IcmpHandler::calculate_checksum(icmp_data, 8);
    EXPECT_NE(checksum, 0);
}

// 测试五元组
TEST(FiveTupleTest, Equality) {
    FiveTuple t1(1, 2, 3, 4, 6);
    FiveTuple t2(1, 2, 3, 4, 6);
    FiveTuple t3(1, 2, 3, 5, 6);
    
    EXPECT_EQ(t1, t2);
    EXPECT_FALSE(t1 == t3);
}

TEST(FiveTupleTest, Reverse) {
    FiveTuple t1(1, 2, 100, 200, 6);
    FiveTuple t2 = t1.reverse();
    
    EXPECT_EQ(t2.src_ip, 2);
    EXPECT_EQ(t2.dst_ip, 1);
    EXPECT_EQ(t2.src_port, 200);
    EXPECT_EQ(t2.dst_port, 100);
}

TEST(FiveTupleTest, Hash) {
    FiveTuple t1(1, 2, 3, 4, 6);
    FiveTuple t2(1, 2, 3, 4, 6);
    
    FiveTupleHash hasher;
    EXPECT_EQ(hasher(t1), hasher(t2));
}

// 测试 IP 转换
TEST(TypesTest, IpConversion) {
    std::string ip_str = "192.168.1.1";
    IPv4Addr ip = ip_from_string(ip_str);
    
    std::string result = ip_to_string(ip);
    EXPECT_EQ(result, ip_str);
}

TEST(TypesTest, MacConversion) {
    std::string mac_str = "00:0C:29:3E:38:92";
    MacAddr mac = mac_from_string(mac_str);
    
    std::string result = mac_to_string(mac);
    EXPECT_EQ(result, mac_str);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
