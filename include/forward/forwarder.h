/**
 * @file forwarder.h
 * @brief 转发引擎接口
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_FORWARD_FORWARDER_H
#define L4LB_FORWARD_FORWARDER_H

#include <cstdint>
#include "common/types.h"
#include "protocol/ethernet.h"
#include "protocol/ip.h"
#include "lb/real_server.h"

namespace l4lb {

/**
 * @brief 转发引擎基类
 */
class Forwarder {
public:
    virtual ~Forwarder() = default;
    
    /**
     * @brief 转发数据包
     * @param pkt 数据包缓冲区
     * @param len 数据包长度
     * @param meta 解析后的元信息
     * @param rs 目标服务器
     * @return true 转发成功
     */
    virtual bool forward(uint8_t* pkt, size_t len, 
                         const PacketMeta& meta, 
                         RealServer* rs) = 0;
    
    /**
     * @brief 处理返回流量
     */
    virtual bool forward_reply(uint8_t* pkt, size_t len,
                               const PacketMeta& meta,
                               const Session& session) = 0;
    
    /**
     * @brief 获取转发模式
     */
    virtual ForwardMode mode() const = 0;
};

} // namespace l4lb

#endif // L4LB_FORWARD_FORWARDER_H
