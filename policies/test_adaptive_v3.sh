#!/bin/bash
set -e

# Adaptive policy v3 테스트 스크립트

echo "=================================="
echo "Adaptive Policy v3 Test Script"
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
if [ ! -f "cache_ext_adaptive_v3.out" ]; then
    echo "Building cache_ext_adaptive_v3..."
    make cache_ext_adaptive_v3.out
    echo ""
fi

echo "✓ cache_ext_adaptive_v3.out exists"
echo ""

# 3. Cgroup 생성
CGROUP_PATH="/sys/fs/cgroup/adaptive_v3_test"
if [ ! -d "$CGROUP_PATH" ]; then
    echo "Creating cgroup at $CGROUP_PATH"
    sudo mkdir -p "$CGROUP_PATH"

    # cgroup v2에서 memory controller 활성화
    if [ -f "/sys/fs/cgroup/cgroup.subtree_control" ]; then
        echo "+memory" | sudo tee /sys/fs/cgroup/cgroup.subtree_control > /dev/null 2>&1 || true
    fi
fi

echo "✓ Cgroup created: $CGROUP_PATH"
echo ""

# 4. 테스트 디렉토리 확인
WATCH_DIR="/mydata/leveldb_db"
if [ ! -d "$WATCH_DIR" ]; then
    echo "Warning: $WATCH_DIR does not exist"

    # 대체 디렉토리 시도
    if [ -d "/mydata" ]; then
        WATCH_DIR="/mydata"
    else
        echo "Using current directory for testing..."
        WATCH_DIR="$(pwd)"
    fi
fi

echo "✓ Watch directory: $WATCH_DIR"
echo ""

# 5. v3의 새 기능 안내
echo "v3 New Features:"
echo "  • 5 policies: MRU, FIFO, LRU, S3-FIFO, LHD-Simple"
echo "  • Working set size tracking"
echo "  • Enhanced metrics (9 total)"
echo ""

# 6. 정책 실행
echo "Starting adaptive policy v3..."
echo "  Watch dir: $WATCH_DIR"
echo "  Cgroup:    $CGROUP_PATH"
echo ""
echo "Run workload_test_v3.sh in another terminal to trigger policy switches."
echo ""
echo "Press Ctrl-C to stop."
echo ""

sudo ./cache_ext_adaptive_v3.out \
    --watch_dir "$WATCH_DIR" \
    --cgroup_path "$CGROUP_PATH"
