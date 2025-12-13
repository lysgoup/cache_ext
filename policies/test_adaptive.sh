#!/bin/bash
set -e

# Adaptive policy 테스트 스크립트

echo "=================================="
echo "Adaptive Policy Test Script"
echo "=================================="
echo ""

# 1. 커널 버전 확인
if ! uname -r | grep -q "cache-ext"; then
    echo "ERROR: Not running cache_ext kernel!"
    echo "Please boot into cache_ext kernel first."
    exit 1
fi

echo "✓ Running cache_ext kernel: $(uname -r)"
echo ""

# 2. 빌드 확인
if [ ! -f "cache_ext_adaptive.out" ]; then
    echo "Building cache_ext_adaptive..."
    make cache_ext_adaptive.out
    echo ""
fi

echo "✓ cache_ext_adaptive.out exists"
echo ""

# 3. Cgroup 생성
CGROUP_PATH="/sys/fs/cgroup/adaptive_test"
if [ ! -d "$CGROUP_PATH" ]; then
    echo "Creating cgroup at $CGROUP_PATH"
    sudo mkdir -p "$CGROUP_PATH"
fi

echo "✓ Cgroup created: $CGROUP_PATH"
echo ""

# 4. 테스트 디렉토리 확인
WATCH_DIR="/mydata/leveldb_db"
if [ ! -d "$WATCH_DIR" ]; then
    echo "Warning: $WATCH_DIR does not exist"
    echo "Using current directory for testing..."
    WATCH_DIR="."
fi

echo "✓ Watch directory: $WATCH_DIR"
echo ""

# 5. 정책 실행
echo "Starting adaptive policy..."
echo "  Watch dir: $WATCH_DIR"
echo "  Cgroup:    $CGROUP_PATH"
echo ""
echo "Press Ctrl-C to stop."
echo ""

sudo ./cache_ext_adaptive.out \
    --watch_dir "$WATCH_DIR" \
    --cgroup_path "$CGROUP_PATH"
