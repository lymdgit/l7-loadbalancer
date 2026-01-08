cmake_minimum_required(VERSION 3.16)
project(l4-loadbalancer VERSION 1.0.0 LANGUAGES C CXX)

# ============================================================================
# 项目说明：基于 DPDK/F-Stack 的四层负载均衡器
# 作者：针对腾讯 C/C++ 后端岗位的项目实践
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# F-Stack 和 DPDK 配置
# ============================================================================
set(FSTACK_PATH "/data/f-stack" CACHE PATH "F-Stack installation path")
set(DPDK_PATH "${FSTACK_PATH}/dpdk" CACHE PATH "DPDK installation path")

# DPDK 库路径 - 注意：库文件在 build/lib 目录下
set(DPDK_LIB_PATH "${DPDK_PATH}/build/lib")
set(DPDK_INCLUDE_PATH "${DPDK_PATH}/include")

# F-Stack 库路径
set(FSTACK_LIB_PATH "${FSTACK_PATH}/lib")
set(FSTACK_INCLUDE_PATH "${FSTACK_PATH}/lib")

# ============================================================================
# 编译选项
# ============================================================================
add_compile_options(
    -Wall                   # 开启所有警告
    -Wextra                 # 开启额外警告
    -O2                     # 优化级别
    -march=native           # 针对本机 CPU 优化
    -g                      # 调试信息
)

# DPDK 需要的宏定义
add_definitions(
    -DALLOW_EXPERIMENTAL_API
    -D_GNU_SOURCE
)

# ============================================================================
# 包含目录
# ============================================================================
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${DPDK_INCLUDE_PATH}
    ${FSTACK_INCLUDE_PATH}
)

# ============================================================================
# 源文件收集
# ============================================================================
file(GLOB_RECURSE COMMON_SOURCES  "src/common/*.cpp")
file(GLOB_RECURSE CORE_SOURCES    "src/core/*.cpp")
file(GLOB_RECURSE PROTOCOL_SOURCES "src/protocol/*.cpp")
file(GLOB_RECURSE LB_SOURCES      "src/lb/*.cpp")
file(GLOB_RECURSE FORWARD_SOURCES "src/forward/*.cpp")

set(ALL_SOURCES
    src/main.cpp
    ${COMMON_SOURCES}
    ${CORE_SOURCES}
    ${PROTOCOL_SOURCES}
    ${LB_SOURCES}
    ${FORWARD_SOURCES}
)

# ============================================================================
# 主程序目标
# ============================================================================
add_executable(l4lb ${ALL_SOURCES})

# ============================================================================
# 链接 DPDK 库
# 注意：手动指定 DPDK 库，不依赖 pkg-config
# ============================================================================

# 添加 DPDK 库搜索路径
link_directories(${DPDK_LIB_PATH})

# F-Stack 库
find_library(FSTACK_LIB fstack PATHS ${FSTACK_LIB_PATH} /usr/local/lib REQUIRED)

# DPDK 核心库列表（按依赖顺序）
# 注意：这些库名可能需要根据实际安装情况调整
set(DPDK_LIBS
    # 使用 whole-archive 确保所有符号被包含
    -Wl,--whole-archive
    rte_eal
    rte_log              # 日志库
    rte_mempool
    rte_mempool_ring
    rte_ring
    rte_mbuf
    rte_ethdev
    rte_net
    rte_net_bond         # Bonding 驱动 - F-Stack 需要
    rte_kvargs
    rte_hash
    rte_rcu
    rte_timer
    rte_cmdline
    rte_telemetry
    rte_pci
    rte_bus_pci
    rte_bus_vdev
    rte_net_vmxnet3    # VMware 网卡驱动
    rte_net_e1000      # Intel 网卡驱动
    rte_net_virtio     # Virtio 网卡驱动
    -Wl,--no-whole-archive
)

# 添加共享库路径
link_directories(/usr/local/lib64)

# 链接库
target_link_libraries(l4lb
    ${FSTACK_LIB}
    ${DPDK_LIBS}
    pthread
    dl
    numa
    m
    crypto              # OpenSSL crypto - F-Stack 需要
)

# 添加 DPDK 头文件路径
target_include_directories(l4lb PRIVATE ${DPDK_INCLUDE_PATH})

# ============================================================================
# 测试配置 (可选)
# ============================================================================
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()
    
    # 下载 Google Test
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    
    # 添加测试可执行文件
    add_executable(test_consistent_hash tests/unit/test_consistent_hash.cpp)
    target_link_libraries(test_consistent_hash GTest::gtest_main)
    target_include_directories(test_consistent_hash PRIVATE ${CMAKE_SOURCE_DIR}/include)
    
    add_executable(test_ring_buffer tests/unit/test_ring_buffer.cpp)
    target_link_libraries(test_ring_buffer GTest::gtest_main pthread)
    target_include_directories(test_ring_buffer PRIVATE ${CMAKE_SOURCE_DIR}/include)
    
    add_executable(test_protocol tests/unit/test_protocol.cpp)
    target_link_libraries(test_protocol GTest::gtest_main)
    target_include_directories(test_protocol PRIVATE ${CMAKE_SOURCE_DIR}/include)
    
    include(GoogleTest)
    gtest_discover_tests(test_consistent_hash)
    gtest_discover_tests(test_ring_buffer)
    gtest_discover_tests(test_protocol)
endif()

# ============================================================================
# 安装配置
# ============================================================================
install(TARGETS l4lb RUNTIME DESTINATION bin)
install(FILES config/lb.conf DESTINATION etc/l4lb)

# ============================================================================
# 输出配置信息
# ============================================================================
message(STATUS "====================================")
message(STATUS "L4 Load Balancer Configuration:")
message(STATUS "  F-Stack Path: ${FSTACK_PATH}")
message(STATUS "  DPDK Path: ${DPDK_PATH}")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
message(STATUS "====================================")
