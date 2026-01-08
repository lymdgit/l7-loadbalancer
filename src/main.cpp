/**
 * @file main.cpp
 * @brief L7 TCP 代理负载均衡器主程序 - 代理模式
 * 
 * 使用 F-Stack 的 socket API 实现 L7 TCP 代理：
 * 1. 在 VIP 上监听
 * 2. 接受客户端连接
 * 3. 根据一致性哈希选择后端服务器
 * 4. 建立到后端的连接
 * 5. 在客户端和后端之间转发数据
 * 
 * @author L7 TCP Proxy Load Balancer Project
 */

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern "C" {
#include <ff_api.h>
#include <ff_config.h>
#include <ff_epoll.h>
}

#include "common/config.h"
#include "common/logger.h"
#include "common/types.h"
#include "lb/consistent_hash.h"
#include "lb/real_server.h"

using namespace l4lb;

// 全局变量
static volatile bool g_running = true;
static std::string g_config_file;
static int g_epfd = -1;
static int g_listen_fd = -1;
static ConsistentHashRing g_hash_ring(150);
static Statistics g_stats{};

// 连接上下文
struct Connection {
    int client_fd;
    int backend_fd;
    uint32_t server_id;
    bool client_connected;
    bool backend_connected;
    
    // 缓冲区
    char client_buf[4096];
    char backend_buf[4096];
    int client_buf_len;
    int backend_buf_len;
};

// 连接映射
static std::unordered_map<int, Connection*> g_connections;

/**
 * @brief 信号处理函数
 */
static void signal_handler(int sig) {
    (void)sig;
    LOG_INFO("Received signal %d, shutting down...", sig);
    g_running = false;
}

/**
 * @brief 创建非阻塞 socket
 */
static int create_socket() {
    int fd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("Failed to create socket: %d", fd);
        return -1;
    }
    
    // 设置非阻塞
    int flags = ff_fcntl(fd, F_GETFL, 0);
    ff_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    // 设置 SO_REUSEADDR
    int opt = 1;
    ff_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    return fd;
}

/**
 * @brief 创建监听 socket
 */
static int create_listen_socket(uint16_t port) {
    int fd = create_socket();
    if (fd < 0) return -1;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (ff_bind(fd, (struct linux_sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind port %u", port);
        ff_close(fd);
        return -1;
    }
    
    if (ff_listen(fd, 1024) < 0) {
        LOG_ERROR("Failed to listen");
        ff_close(fd);
        return -1;
    }
    
    LOG_INFO("Listening on port %u", port);
    return fd;
}

/**
 * @brief 连接到后端服务器
 */
static int connect_to_backend(RealServer* rs) {
    int fd = create_socket();
    if (fd < 0) return -1;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = rs->ip;  // 已经是网络字节序
    addr.sin_port = htons(rs->port);
    
    int ret = ff_connect(fd, (struct linux_sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        LOG_ERROR("Failed to connect to backend %s:%u", 
                  ip_to_string(rs->ip).c_str(), rs->port);
        ff_close(fd);
        return -1;
    }
    
    LOG_DEBUG("Connecting to backend %s:%u fd=%d",
              ip_to_string(rs->ip).c_str(), rs->port, fd);
    return fd;
}

/**
 * @brief 处理新连接
 */
static void handle_accept(int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    
    int client_fd = ff_accept(listen_fd, (struct linux_sockaddr*)&client_addr, &addrlen);
    if (client_fd < 0) {
        return;
    }
    
    // 设置非阻塞
    int flags = ff_fcntl(client_fd, F_GETFL, 0);
    ff_fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    
    LOG_INFO("New connection from %s:%u fd=%d",
             inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), client_fd);
    
    // 构建五元组
    FiveTuple tuple;
    tuple.src_ip = client_addr.sin_addr.s_addr;
    tuple.src_port = client_addr.sin_port;
    tuple.dst_ip = Config::instance().get_vip();
    tuple.dst_port = htons(8080);  // TODO: 从实际端口获取
    tuple.protocol = 6;
    
    // 使用一致性哈希选择后端服务器
    auto* rs = RealServerManager::instance().select_server(tuple);
    if (!rs) {
        LOG_WARN("No available backend server");
        ff_close(client_fd);
        return;
    }
    
    LOG_INFO("Selected backend server: %s:%u", 
             ip_to_string(rs->ip).c_str(), rs->port);
    
    // 连接到后端
    int backend_fd = connect_to_backend(rs);
    if (backend_fd < 0) {
        ff_close(client_fd);
        return;
    }
    
    // 创建连接上下文
    Connection* conn = new Connection();
    conn->client_fd = client_fd;
    conn->backend_fd = backend_fd;
    conn->server_id = rs->id;
    conn->client_connected = true;
    conn->backend_connected = false;  // 等待连接完成
    conn->client_buf_len = 0;
    conn->backend_buf_len = 0;
    
    g_connections[client_fd] = conn;
    g_connections[backend_fd] = conn;
    
    // 添加到 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = client_fd;
    ff_epoll_ctl(g_epfd, EPOLL_CTL_ADD, client_fd, &ev);
    
    ev.events = EPOLLIN | EPOLLOUT;  // 等待连接完成
    ev.data.fd = backend_fd;
    ff_epoll_ctl(g_epfd, EPOLL_CTL_ADD, backend_fd, &ev);
    
    ++g_stats.active_sessions;
    ++g_stats.total_sessions;
}

/**
 * @brief 关闭连接
 */
static void close_connection(Connection* conn) {
    if (!conn) return;
    
    LOG_DEBUG("Closing connection client_fd=%d backend_fd=%d",
              conn->client_fd, conn->backend_fd);
    
    if (conn->client_fd > 0) {
        ff_epoll_ctl(g_epfd, EPOLL_CTL_DEL, conn->client_fd, NULL);
        ff_close(conn->client_fd);
        g_connections.erase(conn->client_fd);
    }
    
    if (conn->backend_fd > 0) {
        ff_epoll_ctl(g_epfd, EPOLL_CTL_DEL, conn->backend_fd, NULL);
        ff_close(conn->backend_fd);
        g_connections.erase(conn->backend_fd);
    }
    
    delete conn;
    --g_stats.active_sessions;
}

/**
 * @brief 转发数据 - 返回 false 表示连接应该关闭
 */
static bool forward_data(Connection* conn, int from_fd, int to_fd, bool& from_closed) {
    char buf[8192];
    from_closed = false;
    
    ssize_t n = ff_read(from_fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 没有更多数据可读
            return true;  // 继续保持连接
        }
        // 读取错误
        LOG_INFO("Read error on fd=%d errno=%d", from_fd, errno);
        return false;
    }
    if (n == 0) {
        // 对端关闭连接
        LOG_INFO("Peer closed fd=%d", from_fd);
        from_closed = true;
        return true;  // 不立即返回 false，让另一方继续处理
    }
    
    LOG_INFO("Read %zd bytes from fd=%d", n, from_fd);
    
    // 写入对端
    ssize_t total_written = 0;
    while (total_written < n) {
        ssize_t written = ff_write(to_fd, buf + total_written, n - total_written);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // TODO: 应该缓存未发送的数据，这里简化处理
                break;
            }
            LOG_INFO("Write error on fd=%d errno=%d", to_fd, errno);
            return false;
        }
        total_written += written;
    }
    
    LOG_INFO("Wrote %zd bytes to fd=%d", total_written, to_fd);
    
    if (from_fd == conn->client_fd) {
        ++g_stats.rx_packets;
        ++g_stats.forwarded_packets;
    } else {
        ++g_stats.tx_packets;
    }
    
    return true;
}

/**
 * @brief 处理事件
 */
static void handle_event(struct epoll_event* ev) {
    int fd = ev->data.fd;
    
    // 处理监听 socket
    if (fd == g_listen_fd) {
        handle_accept(fd);
        return;
    }
    
    // 查找连接
    auto it = g_connections.find(fd);
    if (it == g_connections.end()) {
        return;
    }
    
    Connection* conn = it->second;
    
    // 检查错误 (但不是 EPOLLHUP，那是正常关闭)
    if (ev->events & EPOLLERR) {
        LOG_INFO("Connection error on fd=%d", fd);
        close_connection(conn);
        return;
    }
    
    // 处理后端连接完成
    if (fd == conn->backend_fd && !conn->backend_connected) {
        if (ev->events & EPOLLOUT) {
            conn->backend_connected = true;
            LOG_INFO("Backend connected fd=%d", fd);
        } else {
            // 后端还未连接，等待
            return;
        }
    }
    
    // 转发数据
    if (ev->events & EPOLLIN) {
        bool peer_closed = false;
        
        if (fd == conn->client_fd) {
            // 客户端有数据 -> 转发到后端
            if (conn->backend_connected) {
                LOG_INFO("Client->Backend: fd %d -> %d", conn->client_fd, conn->backend_fd);
                if (!forward_data(conn, conn->client_fd, conn->backend_fd, peer_closed)) {
                    close_connection(conn);
                    return;
                }
                if (peer_closed) {
                    // 客户端关闭，可以关闭整个连接
                    close_connection(conn);
                    return;
                }
            } else {
                LOG_INFO("Waiting for backend connection...");
            }
        } else if (fd == conn->backend_fd) {
            // 后端有数据 -> 转发到客户端
            LOG_INFO("Backend->Client: fd %d -> %d", conn->backend_fd, conn->client_fd);
            if (!forward_data(conn, conn->backend_fd, conn->client_fd, peer_closed)) {
                close_connection(conn);
                return;
            }
            if (peer_closed) {
                // 后端关闭是正常的 HTTP 行为，但先不关闭客户端
                // 标记后端已关闭
                LOG_INFO("Backend closed normally, keeping client connection");
            }
        }
    }
    
    // 处理挂起 - 对于后端是正常的
    if (ev->events & EPOLLHUP) {
        if (fd == conn->backend_fd) {
            // 后端关闭是预期的
            LOG_INFO("Backend hangup fd=%d - normal for HTTP", fd);
        } else {
            // 客户端挂起，关闭连接
            LOG_INFO("Client hangup fd=%d", fd);
            close_connection(conn);
        }
    }
}

/**
 * @brief F-Stack 主循环
 */
static int ff_loop(void* arg) {
    (void)arg;
    
    struct epoll_event events[64];
    int n = ff_epoll_wait(g_epfd, events, 64, 0);
    
    for (int i = 0; i < n; ++i) {
        handle_event(&events[i]);
    }
    
    // 定期打印统计
    static uint64_t loop_count = 0;
    if (++loop_count % 100000 == 0) {
        LOG_INFO("Stats: Sessions=%lu Total=%lu RX=%lu TX=%lu FWD=%lu",
                 g_stats.active_sessions, g_stats.total_sessions,
                 g_stats.rx_packets, g_stats.tx_packets,
                 g_stats.forwarded_packets);
    }
    
    return g_running ? 0 : -1;
}

int main(int argc, char* argv[]) {
    std::string config_file = "config/lb.conf";
    std::string log_level = "info";
    
    // 解析参数
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--lb-config") == 0 && i + 1 < argc) {
            config_file = argv[i + 1];
            argv[i] = argv[i + 1] = (char*)"";
            ++i;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_level = argv[i + 1];
            argv[i] = argv[i + 1] = (char*)"";
            ++i;
        } else if (strcmp(argv[i], "--help-lb") == 0) {
            printf("L7 TCP Proxy Load Balancer - F-Stack\n");
            printf("Usage: %s [F-Stack options] [LB options]\n\n", argv[0]);
            printf("  --lb-config <file>   LB config file\n");
            printf("  --log <level>        Log level\n");
            return 0;
        }
    }
    
    // 重建 argv
    std::vector<char*> new_argv;
    for (int i = 0; i < argc; ++i) {
        if (strlen(argv[i]) > 0) {
            new_argv.push_back(argv[i]);
        }
    }
    int new_argc = static_cast<int>(new_argv.size());
    
    Logger::instance().set_level(log_level);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    LOG_INFO("========================================");
    LOG_INFO("L7 TCP Proxy Load Balancer starting...");
    LOG_INFO("Config: %s", config_file.c_str());
    LOG_INFO("========================================");
    
    // 初始化 F-Stack
    if (ff_init(new_argc, new_argv.data()) < 0) {
        LOG_FATAL("Failed to initialize F-Stack");
        return 1;
    }
    LOG_INFO("F-Stack initialized");
    
    // 加载配置
    if (!Config::instance().load(config_file)) {
        LOG_FATAL("Failed to load config");
        return 1;
    }
    Config::instance().dump();
    
    // 加载后端服务器
    if (!RealServerManager::instance().load_from_config()) {
        LOG_FATAL("Failed to load real servers");
        return 1;
    }
    
    // 创建 epoll
    g_epfd = ff_epoll_create(1024);
    if (g_epfd < 0) {
        LOG_FATAL("Failed to create epoll");
        return 1;
    }
    
    // 创建监听 socket (端口 8080)
    g_listen_fd = create_listen_socket(8080);
    if (g_listen_fd < 0) {
        return 1;
    }
    
    // 添加监听 socket 到 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_listen_fd;
    ff_epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &ev);
    
    LOG_INFO("Load balancer started, listening on VIP:8080");
    LOG_INFO("Use 'sudo pkill -9 l4lb' to stop");
    
    // 主循环
    ff_run(ff_loop, NULL);
    
    LOG_INFO("Load balancer stopped");
    LOG_INFO("Final stats: Sessions=%lu RX=%lu TX=%lu FWD=%lu",
             g_stats.total_sessions, g_stats.rx_packets,
             g_stats.tx_packets, g_stats.forwarded_packets);
    
    return 0;
}
