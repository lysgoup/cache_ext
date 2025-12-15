#!/bin/bash

# Adaptive v2.1 테스트 스크립트

CGROUP_PATH="/sys/fs/cgroup/adaptive_v2_1_test"
WATCH_DIR="${1:-/mydata/adaptive_test_data}"

echo "=========================================="
echo "Adaptive v2.1 Test Script"
echo "=========================================="
echo ""
echo "This script helps you run adaptive_v2_1 with proper setup."
echo ""

# Cgroup 생성
if [ ! -d "$CGROUP_PATH" ]; then
    echo "Creating cgroup: $CGROUP_PATH"
    sudo mkdir -p "$CGROUP_PATH"
    if [ $? -ne 0 ]; then
        echo "Error: Failed to create cgroup"
        echo "Make sure you have sudo privileges"
        exit 1
    fi
    echo "✓ Cgroup created"
else
    echo "✓ Cgroup already exists: $CGROUP_PATH"
fi

# Memory limit 설정 (200MB)
echo ""
echo "Setting memory limit to 200MB..."
echo $((200 * 1024 * 1024)) | sudo tee "$CGROUP_PATH/memory.max" > /dev/null
if [ $? -eq 0 ]; then
    echo "✓ Memory limit set to 200MB"
else
    echo "Warning: Failed to set memory limit (may not be critical)"
fi

# Watch directory 확인
echo ""
if [ ! -d "$WATCH_DIR" ]; then
    echo "Error: Watch directory does not exist: $WATCH_DIR"
    echo ""
    echo "Please create test data first:"
    echo "  cd /home/yunseo/project/cache_ext"
    echo "  ./create_test_data.sh"
    echo ""
    echo "Or specify a different directory:"
    echo "  $0 /path/to/your/data"
    exit 1
fi

echo "✓ Watch directory exists: $WATCH_DIR"

# 실행 파일 확인
echo ""
if [ ! -f "./cache_ext_adaptive_v2_1.out" ]; then
    echo "Error: cache_ext_adaptive_v2_1.out not found"
    echo ""
    echo "Please build first:"
    echo "  make cache_ext_adaptive_v2_1.out"
    exit 1
fi

echo "✓ Executable found: cache_ext_adaptive_v2_1.out"

# 설정 요약
echo ""
echo "=========================================="
echo "Configuration Summary"
echo "=========================================="
echo "  Cgroup:        $CGROUP_PATH"
echo "  Watch dir:     $WATCH_DIR"
echo "  Memory limit:  200MB"
echo ""
echo "Architecture:"
echo "  ✓ Single unified list"
echo "  ✓ Per-inode sequential tracking"
echo "  ✓ Relaxed thresholds (MIN_SAMPLES=200)"
echo ""

# 사용법 안내
echo "=========================================="
echo "Usage Instructions"
echo "=========================================="
echo ""
echo "1. This terminal will run adaptive_v2_1"
echo ""
echo "2. Open another terminal and run:"
echo "   sudo dmesg -wH | grep METRICS"
echo "   → To see periodic metrics (every 100 accesses)"
echo ""
echo "3. Open a third terminal and run workload:"
echo "   cd $(pwd)"
echo "   ./workload_test.sh $CGROUP_PATH $WATCH_DIR"
echo ""
echo "4. Watch for:"
echo "   - Policy switch events (this terminal)"
echo "   - Periodic metrics (terminal 2)"
echo "   - Workload progress (terminal 3)"
echo ""

# 시작 확인
echo "=========================================="
echo ""
read -p "Press Enter to start adaptive_v2_1..."
echo ""

# 실행
echo "Starting adaptive_v2_1..."
echo ""

sudo ./cache_ext_adaptive_v2_1.out \
    --watch_dir "$WATCH_DIR" \
    --cgroup_path "$CGROUP_PATH"

# Cleanup (Ctrl-C 후)
EXIT_CODE=$?

echo ""
echo "=========================================="
echo "Cleaning up..."
echo "=========================================="

# Cgroup 정리 시도
echo "Removing cgroup..."
sudo rmdir "$CGROUP_PATH" 2>/dev/null
if [ $? -eq 0 ]; then
    echo "✓ Cgroup removed"
else
    echo "Note: Cgroup may still have processes, remove manually if needed:"
    echo "  sudo rmdir $CGROUP_PATH"
fi

echo ""
echo "Adaptive v2.1 test completed."
echo ""

exit $EXIT_CODE
