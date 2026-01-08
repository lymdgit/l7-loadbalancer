#!/bin/bash
# ============================================================================
# run_test.sh - 测试运行脚本
# ============================================================================

set -e

cd "$(dirname "$0")/.."

# 构建测试
echo "Building tests..."
mkdir -p build
cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)

echo ""
echo "=========================================="
echo "Running Unit Tests"
echo "=========================================="

# 运行一致性哈希测试
echo ""
echo ">>> Testing Consistent Hash..."
./tests/unit/test_consistent_hash

# 运行无锁队列测试
echo ""
echo ">>> Testing Ring Buffer..."
./tests/unit/test_ring_buffer

# 运行协议解析测试
echo ""
echo ">>> Testing Protocol Parser..."
./tests/unit/test_protocol

echo ""
echo "=========================================="
echo "All tests passed!"
echo "=========================================="
