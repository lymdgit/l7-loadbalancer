/**
 * @file session.h
 * @brief 会话管理
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_LB_SESSION_H
#define L4LB_LB_SESSION_H

#include <unordered_map>
#include <mutex>
#include <chrono>
#include "common/types.h"

namespace l4lb {

/**
 * @brief 会话管理器
 * 
 * 维护连接的会话状态，确保同一连接的所有包发往同一后端
 */
class SessionManager {
public:
    static SessionManager& instance() {
        static SessionManager mgr;
        return mgr;
    }
    
    /**
     * @brief 设置超时时间
     */
    void set_timeout(uint32_t seconds) {
        timeout_sec_ = seconds;
    }
    
    /**
     * @brief 查找会话
     */
    bool lookup(const FiveTuple& tuple, Session& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(tuple);
        if (it != sessions_.end()) {
            // 更新活跃时间
            it->second.touch();
            session = it->second;
            return true;
        }
        return false;
    }
    
    /**
     * @brief 创建会话
     */
    void create(const FiveTuple& client_tuple, uint32_t server_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        Session session;
        session.client_tuple = client_tuple;
        session.real_server_id = server_id;
        session.create_time = get_timestamp();
        session.last_active = session.create_time;
        session.packets = 0;
        session.bytes = 0;
        
        sessions_[client_tuple] = session;
        ++stats_.total_sessions;
        ++stats_.active_sessions;
    }
    
    /**
     * @brief 更新会话统计
     */
    void update_stats(const FiveTuple& tuple, uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(tuple);
        if (it != sessions_.end()) {
            it->second.touch();
            ++it->second.packets;
            it->second.bytes += bytes;
        }
    }
    
    /**
     * @brief 删除会话
     */
    void remove(const FiveTuple& tuple) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.erase(tuple) > 0) {
            --stats_.active_sessions;
        }
    }
    
    /**
     * @brief 清理过期会话
     */
    size_t cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t removed = 0;
        
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (it->second.is_expired(timeout_sec_)) {
                it = sessions_.erase(it);
                ++removed;
                --stats_.active_sessions;
            } else {
                ++it;
            }
        }
        return removed;
    }
    
    /**
     * @brief 获取活跃会话数
     */
    size_t active_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }
    
    /**
     * @brief 获取统计信息
     */
    Statistics get_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }
    
private:
    SessionManager() : timeout_sec_(300) {}
    
    static uint64_t get_timestamp() {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }
    
    mutable std::mutex mutex_;
    std::unordered_map<FiveTuple, Session, FiveTupleHash> sessions_;
    uint32_t timeout_sec_;
    Statistics stats_{};
};

} // namespace l4lb

#endif // L4LB_LB_SESSION_H
