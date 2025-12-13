#!/bin/bash

# Workload 테스트 스크립트 - adaptive policy의 정책 전환을 유도

CGROUP_PATH="${1:-/sys/fs/cgroup/adaptive_test}"
WATCH_DIR="${2:-/mydata/adaptive_test_data}"

echo "=================================="
echo "Adaptive Policy Workload Test"
echo "=================================="
echo ""
echo "This script runs different workloads to trigger policy switches"
echo ""

if [ ! -d "$WATCH_DIR" ]; then
    echo "Warning: $WATCH_DIR does not exist"
    echo "Using /tmp for testing..."
    WATCH_DIR="/tmp"

    # 테스트 파일 생성
    echo "Creating test files..."
    for i in {1..100}; do
        dd if=/dev/urandom of="$WATCH_DIR/testfile_$i" bs=1M count=10 2>/dev/null
    done
fi

echo "Watch directory: $WATCH_DIR"
echo "Cgroup: $CGROUP_PATH"
echo ""

# Workload 1: Sequential scan (히트율 낮음 - 정책 전환 유발)
echo "=== Workload 1: Sequential Scan (Low Hit Rate) ==="
echo "Reading all files once (should trigger policy switch)..."
echo ""

sudo cgexec -g memory:adaptive_test bash -c "
    for file in $WATCH_DIR/*; do
        if [ -f \"\$file\" ]; then
            cat \"\$file\" > /dev/null 2>&1
        fi
    done
"

echo ""
echo "✓ Sequential scan complete"
echo "  Expected: Hit rate should be LOW, may trigger policy switch"
echo ""
sleep 5

# Workload 2: Repeated reads (히트율 높음 - 정책 유지)
echo "=== Workload 2: Repeated Reads (High Hit Rate) ==="
echo "Reading same files repeatedly (should keep current policy)..."
echo ""

# 첫 번째 파일을 반복적으로 읽기
FIRST_FILE=$(find "$WATCH_DIR" -type f | head -1)

if [ -n "$FIRST_FILE" ]; then
    sudo cgexec -g memory:adaptive_test bash -c "
        for i in {1..500}; do
            cat \"$FIRST_FILE\" > /dev/null 2>&1
        done
    "

    echo ""
    echo "✓ Repeated reads complete"
    echo "  Expected: Hit rate should be HIGH, policy should remain stable"
    echo ""
else
    echo "No files found to test"
fi

sleep 5

# Workload 3: Mixed pattern
echo "=== Workload 3: Mixed Pattern ==="
echo "Alternating between sequential and repeated access..."
echo ""

sudo cgexec -g memory:adaptive_test bash -c "
    for round in {1..3}; do
        echo \"Round \$round: Sequential scan\"
        for file in $WATCH_DIR/*; do
            if [ -f \"\$file\" ]; then
                cat \"\$file\" > /dev/null 2>&1
            fi
        done

        echo \"Round \$round: Repeated access\"
        for i in {1..100}; do
            cat \"$FIRST_FILE\" > /dev/null 2>&1
        done
    done
"

echo ""
echo "✓ Mixed pattern complete"
echo "  Expected: Hit rate fluctuates, may trigger multiple policy switches"
echo ""

# 테스트 파일 정리 (임시 파일을 만들었다면)
if [ "$WATCH_DIR" = "/tmp" ]; then
    echo "Cleaning up test files..."
    rm -f /tmp/testfile_*
fi

echo ""
echo "=================================="
echo "Workload test complete!"
echo "=================================="
echo ""
echo "Check the adaptive policy output for policy switch events."
