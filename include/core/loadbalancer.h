/**
 * @file loadbalancer.h
 * @brief 负载均衡器核心类
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CORE_LOADBALANCER_H
#define L4LB_CORE_LOADBALANCER_H

#include <memory>
#include <atomic>
#include "common/types.h"
#include "common/config.h"
#include "common/logger.h"
#include "protocol/ethernet.h"
#include "protocol/arp.h"
#include "protocol/icmp.h"
#include "protocol/ip.h"
#include "lb/real_server.h"
#include "lb/session.h"
#include "forward/forwarder.h"
#include "forward/nat_forwarder.h"
#include "forward/dr_forwarder.h"

namespace l4lb {

/**
 * @brief 负载均衡器核心类
 */
class LoadBalancer {
public:
    LoadBalancer() : running_(false) {}
    
    /**
     * @brief 初始化
     */
    bool init(const std::string& config_file) {
        // 加载配置
        if (!Config::instance().load(config_file)) {
            LOG_ERROR("Failed to load config");
            return false;
        }
        
        auto& cfg = Config::instance();
        cfg.dump();
        
        // 初始化本机信息
        local_ip_ = cfg.get_vip();
        local_mac_ = cfg.get_vip_mac();
        
        // 初始化 Real Server
        if (!RealServerManager::instance().load_from_config()) {
            LOG_ERROR("Failed to load real servers");
            return false;
        }
        
        // 设置会话超时
        SessionManager::instance().set_timeout(cfg.get_session_timeout());
        
        // 创建转发引擎
        if (cfg.get_forward_mode() == ForwardMode::NAT) {
            forwarder_ = std::make_unique<NatForwarder>();
            LOG_INFO("Using NAT forwarding mode");
        } else {
            forwarder_ = std::make_unique<DrForwarder>();
            LOG_INFO("Using DR forwarding mode");
        }
        
        running_ = true;
        LOG_INFO("LoadBalancer initialized");
        return true;
    }
    
    /**
     * @brief 处理数据包
     */
    bool process_packet(void* mbuf, uint8_t* data, size_t len) {
        if (!running_) return false;
        
        ++stats_.rx_packets;
        
        // 解析以太网头
        auto* eth = Ethernet::parse_mutable(data, len);
        if (!eth) {
            ++stats_.dropped_packets;
            return false;
        }
        
        // ARP 处理
        if (eth->is_arp()) {
            return handle_arp(eth, data, len);
        }
        
        // IPv4 处理
        if (eth->is_ipv4()) {
            return handle_ipv4(eth, data, len, mbuf);
        }
        
        ++stats_.dropped_packets;
        return false;
    }
    
    /**
     * @brief 获取统计信息
     */
    Statistics get_stats() const { return stats_; }
    
    /**
     * @brief 停止
     */
    void stop() { running_ = false; }
    
private:
    /**
     * @brief 处理 ARP
     */
    bool handle_arp(EthernetHeader* eth, uint8_t* data, size_t len) {
        if (len < Ethernet::HEADER_SIZE + sizeof(ArpHeader)) {
            return false;
        }
        
        auto* arp = reinterpret_cast<ArpHeader*>(data + Ethernet::HEADER_SIZE);
        ++stats_.arp_packets;
        
        if (ArpHandler::handle(eth, arp, local_ip_, local_mac_)) {
            // 需要发送 ARP 响应
            ++stats_.tx_packets;
            return true;  // 返回 true 表示需要发送
        }
        return false;
    }
    
    /**
     * @brief 处理 IPv4
     */
    bool handle_ipv4(EthernetHeader* eth, uint8_t* data, size_t len, void* mbuf) {
        PacketMeta meta;
        if (!ProtocolParser::parse(data, len, meta)) {
            ++stats_.dropped_packets;
            return false;
        }
        
        auto* ip = reinterpret_cast<IPv4Header*>(data + meta.l3_offset);
        
        // ICMP 处理 (Ping)
        if (ip->is_icmp() && meta.dst_ip == local_ip_) {
            return handle_icmp(eth, ip, data, len, meta);
        }
        
        // 只转发发往 VIP 的 TCP/UDP 流量
        if (meta.dst_ip != local_ip_) {
            LOG_DEBUG("Packet not for VIP, dst=%s, VIP=%s",
                      ip_to_string(meta.dst_ip).c_str(),
                      ip_to_string(local_ip_).c_str());
            return false;  // 不是发给我们的包，不处理
        }
        
        // TCP/UDP 负载均衡
        if (ip->is_tcp()) {
            ++stats_.tcp_packets;
            return handle_loadbalance(eth, data, len, meta, mbuf);
        }
        if (ip->is_udp()) {
            ++stats_.udp_packets;
            return handle_loadbalance(eth, data, len, meta, mbuf);
        }
        
        return false;
    }
    
    /**
     * @brief 处理 ICMP
     */
    bool handle_icmp(EthernetHeader* eth, IPv4Header* ip, 
                     uint8_t* data, size_t len, const PacketMeta& meta) {
        auto* icmp = reinterpret_cast<IcmpHeader*>(data + meta.l4_offset);
        size_t icmp_len = len - meta.l4_offset;
        
        ++stats_.icmp_packets;
        
        if (IcmpHandler::handle_echo_request(icmp, icmp_len)) {
            // 交换 MAC 和 IP
            eth->swap_mac();
            ip->swap_ip();
            IpChecksum::update(ip);
            
            ++stats_.tx_packets;
            return true;
        }
        return false;
    }
    
    /**
     * @brief 负载均衡处理
     */
    bool handle_loadbalance(EthernetHeader* eth, uint8_t* data, 
                             size_t len, const PacketMeta& meta, void* mbuf) {
        FiveTuple tuple = meta.to_five_tuple();
        
        // 1. 查找已有会话
        Session session;
        if (SessionManager::instance().lookup(tuple, session)) {
            // 已有会话，直接转发
            auto* rs = RealServerManager::instance().get_server(session.real_server_id);
            if (rs && forwarder_->forward(data, len, meta, rs)) {
                SessionManager::instance().update_stats(tuple, len);
                ++stats_.forwarded_packets;
                ++stats_.tx_packets;
                return true;
            }
        }
        
        // 2. 新连接，选择后端服务器
        auto* rs = RealServerManager::instance().select_server(tuple);
        if (!rs) {
            LOG_WARN("No available backend server");
            ++stats_.dropped_packets;
            return false;
        }
        
        // 3. 创建会话
        SessionManager::instance().create(tuple, rs->id);
        
        // 4. 转发
        if (forwarder_->forward(data, len, meta, rs)) {
            ++stats_.forwarded_packets;
            ++stats_.nat_translations;
            ++stats_.tx_packets;
            return true;
        }
        
        ++stats_.dropped_packets;
        return false;
    }
    
    std::atomic<bool> running_;
    IPv4Addr local_ip_;
    MacAddr local_mac_;
    std::unique_ptr<Forwarder> forwarder_;
    Statistics stats_{};
};

} // namespace l4lb

#endif // L4LB_CORE_LOADBALANCER_H
