/**
 * @file real_server.h
 * @brief Real Server 管理
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_LB_REAL_SERVER_H
#define L4LB_LB_REAL_SERVER_H

#include <vector>
#include <unordered_map>
#include <mutex>
#include "common/types.h"
#include "common/config.h"
#include "lb/consistent_hash.h"

namespace l4lb {

/**
 * @brief Real Server 管理器
 */
class RealServerManager {
public:
    static RealServerManager& instance() {
        static RealServerManager mgr;
        return mgr;
    }
    
    /**
     * @brief 从配置加载服务器
     */
    bool load_from_config() {
        auto& cfg = Config::instance();
        auto servers = cfg.get_real_servers();
        
        for (size_t i = 0; i < servers.size(); ++i) {
            RealServer rs;
            rs.id = static_cast<uint32_t>(i + 1);
            rs.ip = ip_from_string(servers[i].ip);
            rs.port = servers[i].port;
            rs.mac = mac_from_string(servers[i].mac);
            rs.weight = servers[i].weight;
            rs.status = ServerStatus::UP;
            
            add_server(rs);
        }
        return true;
    }
    
    /**
     * @brief 添加服务器
     */
    void add_server(const RealServer& rs) {
        std::lock_guard<std::mutex> lock(mutex_);
        servers_[rs.id] = rs;
        hash_ring_.add_node(rs.id, rs.weight);
    }
    
    /**
     * @brief 移除服务器
     */
    void remove_server(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        servers_.erase(id);
        hash_ring_.remove_node(id);
    }
    
    /**
     * @brief 设置服务器状态
     */
    void set_status(uint32_t id, ServerStatus status) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = servers_.find(id);
        if (it != servers_.end()) {
            it->second.status = status;
        }
    }
    
    /**
     * @brief 选择服务器
     */
    RealServer* select_server(const FiveTuple& tuple) {
        uint32_t server_id;
        if (!hash_ring_.get_server(tuple, server_id)) {
            return nullptr;
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = servers_.find(server_id);
        if (it == servers_.end() || !it->second.is_available()) {
            return nullptr;
        }
        return &it->second;
    }
    
    /**
     * @brief 获取服务器
     */
    RealServer* get_server(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = servers_.find(id);
        return it != servers_.end() ? &it->second : nullptr;
    }
    
    /**
     * @brief 获取所有服务器
     */
    std::vector<RealServer> get_all_servers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<RealServer> result;
        result.reserve(servers_.size());
        for (const auto& [id, rs] : servers_) {
            result.push_back(rs);
        }
        return result;
    }
    
    /**
     * @brief 获取服务器数量
     */
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return servers_.size();
    }
    
private:
    RealServerManager() : hash_ring_(150) {}
    
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, RealServer> servers_;
    ConsistentHashRing hash_ring_;
};

} // namespace l4lb

#endif // L4LB_LB_REAL_SERVER_H
