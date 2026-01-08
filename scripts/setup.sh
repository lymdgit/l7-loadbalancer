#!/bin/bash
# ============================================================================
# setup.sh - 环境配置脚本
# ============================================================================

set -e

echo "=========================================="
echo "L4 Load Balancer Setup Script"
echo "=========================================="

# 检查 F-Stack
FSTACK_PATH="${FSTACK_PATH:-/data/f-stack}"
if [ ! -d "$FSTACK_PATH" ]; then
    echo "ERROR: F-Stack not found at $FSTACK_PATH"
    echo "Please set FSTACK_PATH environment variable"
    exit 1
fi
echo "F-Stack found at: $FSTACK_PATH"

# 检查 DPDK
DPDK_PATH="${DPDK_PATH:-$FSTACK_PATH/dpdk}"
if [ ! -d "$DPDK_PATH" ]; then
    echo "ERROR: DPDK not found at $DPDK_PATH"
    exit 1
fi
echo "DPDK found at: $DPDK_PATH"

# 设置环境变量
export PKG_CONFIG_PATH="$DPDK_PATH/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="$FSTACK_PATH/lib:$DPDK_PATH/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"

# 创建构建目录
mkdir -p build
cd build

# CMake 配置
echo ""
echo "Running CMake..."
cmake .. \
    -DFSTACK_PATH="$FSTACK_PATH" \
    -DDPDK_PATH="$DPDK_PATH" \
    -DCMAKE_BUILD_TYPE=Release

# 编译
echo ""
echo "Building..."
make -j$(nproc)

echo ""
echo "=========================================="
echo "Build completed successfully!"
echo "Binary: build/l4lb"
echo "=========================================="
