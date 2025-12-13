#!/bin/bash
set -e

# Adaptive policy 테스트용 간단한 데이터셋 생성

TEST_DIR="/mydata/adaptive_test_data"

echo "=================================="
echo "Creating Test Dataset"
echo "=================================="
echo ""
echo "This will create a small dataset optimized for adaptive policy testing"
echo "Location: $TEST_DIR"
echo "Size: ~500MB"
echo ""

read -p "Continue? (y/n) " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    exit 0
fi

# 디렉토리 생성
mkdir -p "$TEST_DIR"/{hot,cold,mixed}

echo "Creating test files..."
echo ""

# 1. Hot files (자주 접근될 파일들) - 작고 많이
echo "1. Creating HOT files (small, frequently accessed)..."
for i in {1..20}; do
    dd if=/dev/urandom of="$TEST_DIR/hot/hot_$i.dat" bs=1M count=2 2>/dev/null
done
echo "   ✓ Created 20 hot files (2MB each, 40MB total)"

# 2. Cold files (한 번만 접근될 파일들) - 크고 적게
echo "2. Creating COLD files (large, one-time access)..."
for i in {1..10}; do
    dd if=/dev/urandom of="$TEST_DIR/cold/cold_$i.dat" bs=1M count=20 2>/dev/null
done
echo "   ✓ Created 10 cold files (20MB each, 200MB total)"

# 3. Mixed files (가끔 접근)
echo "3. Creating MIXED files (medium, occasional access)..."
for i in {1..15}; do
    dd if=/dev/urandom of="$TEST_DIR/mixed/mixed_$i.dat" bs=1M count=10 2>/dev/null
done
echo "   ✓ Created 15 mixed files (10MB each, 150MB total)"

# 4. Sequential scan용 큰 파일
echo "4. Creating SEQUENTIAL scan file..."
dd if=/dev/urandom of="$TEST_DIR/large_sequential.dat" bs=1M count=100 2>/dev/null
echo "   ✓ Created 1 large file (100MB)"

echo ""
echo "=================================="
echo "Test dataset created!"
echo "=================================="
echo ""
echo "Location: $TEST_DIR"
echo "Total size: ~490MB"
echo ""
echo "Structure:"
echo "  hot/    - 20 small files (for MRU policy testing)"
echo "  cold/   - 10 large files (for FIFO policy testing)"
echo "  mixed/  - 15 medium files (for S3-FIFO/LRU testing)"
echo "  large_sequential.dat - For sequential scan testing"
echo ""
echo "Use with adaptive policy:"
echo "  sudo ./cache_ext_adaptive_v3.out \\"
echo "    --watch_dir $TEST_DIR \\"
echo "    --cgroup_path /sys/fs/cgroup/adaptive_v3_test"
echo ""
