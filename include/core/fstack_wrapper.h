/**
 * @file fstack_wrapper.h
 * @brief F-Stack 封装层
 * 
 * 封装 F-Stack API，提供统一的数据包收发接口
 * 
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CORE_FSTACK_WRAPPER_H
#define L4LB_CORE_FSTACK_WRAPPER_H

#include <cstdint>
#include <cstring>
#include <functional>

// F-Stack 头文件
extern "C" {
#include <ff_api.h>
#include <ff_config.h>
}

#include "common/types.h"
#include "common/logger.h"

namespace l4lb {

/**
 * @brief F-Stack 包装器
 */
class FStackWrapper {
public:
    /// 数据包处理回调
    using PacketHandler = std::function<void(void* mbuf, uint8_t* data, size_t len)>;
    
    /**
     * @brief 初始化 F-Stack
     */
    static bool init(int argc, char* argv[]) {
        int ret = ff_init(argc, argv);
        if (ret < 0) {
            LOG_ERROR("F-Stack init failed: %d", ret);
            return false;
        }
        LOG_INFO("F-Stack initialized successfully");
        return true;
    }
    
    /**
     * @brief 运行主循环
     */
    static void run(PacketHandler handler) {
        packet_handler_ = handler;
        ff_run(loop_func, nullptr);
    }
    
    /**
     * @brief 发送数据包（零拷贝）
     */
    static int send_packet(void* mbuf) {
        return ff_sendmsg_buf(mbuf, 0);
    }
    
    /**
     * @brief 获取本机 MAC 地址
     */
    static MacAddr get_local_mac() {
        MacAddr mac{};
        // 从 F-Stack 配置获取
        return mac;
    }
    
private:
    static PacketHandler packet_handler_;
    
    static int loop_func(void* arg) {
        (void)arg;
        
        void* mbuf;
        uint8_t* data;
        uint16_t len;
        
        // 接收数据包
        while (ff_recv(&mbuf, (char**)&data, &len) > 0) {
            if (packet_handler_) {
                packet_handler_(mbuf, data, len);
            }
        }
        
        return 0;
    }
};

// 静态成员初始化
inline FStackWrapper::PacketHandler FStackWrapper::packet_handler_;

} // namespace l4lb

#endif // L4LB_CORE_FSTACK_WRAPPER_H
