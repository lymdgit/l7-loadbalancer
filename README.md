# L7 TCP Proxy Load Balancer

基于 DPDK/F-Stack 的高性能七层 TCP 代理负载均衡器

对标产品：HAProxy / Nginx (代理模式)

## 🚀 特性

- **代理模式** - 使用 Socket API 实现 TCP 连接代理，完整的连接管理
- **一致性哈希** - 基于五元组的智能流量分发，虚拟节点保证均匀分布
- **连接复用** - 高效的客户端和后端连接管理，支持长连接
- **高性能转发** - 基于 F-Stack 用户态协议栈，零内核切换
- **无锁队列** - 多核间高效数据传递，SPSC/MPMC 支持

## 📁 项目结构

```
l4-loadbalancer/
├── CMakeLists.txt              # CMake 构建配置
├── config/
│   └── lb.conf                 # 负载均衡器配置文件
├── include/                    # 头文件
│   ├── common/                 # 公共类型和工具
│   │   ├── types.h             # 核心数据结构
│   │   ├── config.h            # 配置管理
│   │   └── logger.h            # 日志系统
│   ├── protocol/               # 协议处理
│   │   ├── ethernet.h          # 以太网帧
│   │   └── ip.h                # IP/TCP/UDP
│   ├── lb/                     # 负载均衡核心
│   │   ├── consistent_hash.h   # 一致性哈希
│   │   ├── real_server.h       # RS 管理
│   │   └── session.h           # 会话管理
│   ├── forward/                # 转发引擎
│   │   └── forwarder.h         # 接口定义
│   └── core/                   # 核心模块
│       ├── fstack_wrapper.h    # F-Stack 封装
│       ├── ring_buffer.h       # 无锁队列
│       └── loadbalancer.h      # LB 核心类
├── src/
│   └── main.cpp                # 程序入口
├── tests/                      # 测试用例
│   └── unit/
│       ├── test_consistent_hash.cpp
│       ├── test_ring_buffer.cpp
│       └── test_protocol.cpp
└── scripts/
    ├── setup.sh                # 环境配置
    └── run_test.sh             # 运行测试
```

## 🔧 环境要求

- Ubuntu 18.04/20.04/22.04
- F-Stack (已安装在 `/data/f-stack`)
- DPDK 23.11.x (已安装在 `/data/f-stack/dpdk`)
- GCC 7+ 或 Clang 6+
- CMake 3.16+

## 📦 快速开始

### 1. 编译

```bash
cd l4-loadbalancer

# 设置环境变量
export FSTACK_PATH=/data/f-stack
export DPDK_PATH=/data/f-stack/dpdk
export PKG_CONFIG_PATH=$DPDK_PATH/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
export LD_LIBRARY_PATH=$FSTACK_PATH/lib:$DPDK_PATH/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

# 编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

或使用脚本：

```bash
chmod +x scripts/setup.sh
./scripts/setup.sh
```

### 2. 配置

修改 `config/lb.conf`：

```ini
[global]
mode = proxy                    # 转发模式: proxy (代理模式)
log_level = info

[vip]
ip = 192.168.72.160             # VIP 地址
ports = 80,8080                 # 监听端口
mac = 00:0C:29:3E:38:92         # 本机 MAC

[realserver]
count = 2
server1 = 192.168.72.161:8080:100:00:00:00:00:00:00
server2 = 192.168.72.162:8080:100:00:00:00:00:00:00

[network]
gateway = 192.168.72.1
netmask = 255.255.255.0
```

### 3. 运行

```bash

# 或使用原有的 F-Stack 配置
sudo ./l4lb -c /data/f-stack/example/config.ini --lb-config ../config/lb.conf
```

### 4. 测试

#### Ping 测试

```bash
# 从另一台机器
ping 192.168.72.160
```

#### HTTP 负载均衡测试

```bash
# 在后端服务器启动 HTTP 服务
# RS1: python3 -m http.server 8080
# RS2: python3 -m http.server 8080

# 访问 VIP
curl http://192.168.72.160:8080/
```

## 🧪 单元测试

```bash
cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)

# 运行测试
./tests/unit/test_consistent_hash
./tests/unit/test_ring_buffer
./tests/unit/test_protocol

# 或使用脚本
./scripts/run_test.sh
```

## 🏗️ 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                    应用层 (Application)                       │
│                 main.cpp, 配置解析, 日志                       │
├─────────────────────────────────────────────────────────────┤
│                  负载均衡层 (Load Balancer)                    │
│          一致性哈希, RS管理, 会话保持, 健康检查                   │
├─────────────────────────────────────────────────────────────┤
│                 连接管理层 (Connection Management)             │
│         TCP连接代理, 客户端/后端连接映射, 数据转发                 │
├─────────────────────────────────────────────────────────────┤
│                   协议层 (Protocol Layer)                     │
│              Socket API, TCP连接处理, epoll事件管理             │
├─────────────────────────────────────────────────────────────┤
│                    基础层 (Foundation)                        │
│            F-Stack封装, 无锁队列, 内存池管理                     │
└─────────────────────────────────────────────────────────────┘
```

## 📊 技术亮点

### 1. 一致性哈希

- 使用 MurmurHash3 作为哈希函数
- 虚拟节点技术保证负载均衡
- 节点变更时最小化流量重分布

### 2. 无锁队列

- SPSC 模式：单生产者单消费者，最高性能
- MPMC 模式：多生产者多消费者，灵活性强
- 缓存行对齐避免伪共享

### 3. TCP 连接代理

- 使用 F-Stack Socket API (ff_accept, ff_connect, ff_read, ff_write)
- 终止客户端连接，建立新的后端连接
- 独立管理客户端和后端连接状态
- 非阻塞 I/O 和 epoll 事件驱动

### 4. 连接管理

- 维护客户端到后端的连接映射
- 支持连接复用和长连接
- 自动处理连接超时和异常

## 📝 面试要点

1. **为什么使用一致性哈希？**
   - 节点增减时只影响相邻节点
   - 保持会话亲和性

2. **无锁队列如何保证线程安全？**
   - CAS 操作保证原子性
   - memory_order 保证可见性

3. **L7 代理模式的工作原理？**
   - 终止客户端 TCP 连接（accept）
   - 选择后端服务器并建立新连接（connect）
   - 在两个独立连接间转发数据（read/write）
   - 维护连接状态和缓冲区

4. **L4 和 L7 负载均衡的区别？**
   - L4：工作在网络层，转发 IP 数据包，修改 IP/MAC 地址
   - L7：工作在应用层，代理 TCP 连接，可以解析应用协议
   - L7 可以实现更复杂的路由策略和内容感知

5. **为什么选择 F-Stack？**
   - 用户态协议栈，零内核切换
   - 兼容 POSIX Socket API
   - 基于 DPDK，高性能网络处理

## 📜 License

MIT License
