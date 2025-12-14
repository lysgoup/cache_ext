#!/bin/bash

# Workload 테스트 스크립트 - adaptive policy의 정책 전환을 유도

CGROUP_PATH="${1:-/sys/fs/cgroup/adaptive_test}"
WATCH_DIR="${2:-/mydata/adaptive_test_data}"

# Extract cgroup name from path (e.g., /sys/fs/cgroup/adaptive_v2_test -> adaptive_v2_test)
CGROUP_NAME=$(basename "$CGROUP_PATH")

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
echo "Reading all files MULTIPLE TIMES to trigger eviction and policy switch..."
echo ""

sudo cgexec -g memory:$CGROUP_NAME bash -c "
    echo 'Starting HEAVY sequential scans (20 rounds for eviction)...'
    for round in {1..20}; do
        for file in $WATCH_DIR/cold/*.dat $WATCH_DIR/mixed/*.dat; do
            if [ -f \"\$file\" ]; then
                cat \"\$file\" > /dev/null 2>&1
            fi
        done
    done
    echo 'Sequential scan complete - eviction should have happened'
"

echo ""
echo "✓ Sequential scan complete"
echo "  Expected: High eviction due to memory limit, policy MUST switch to FIFO"
echo ""
sleep 3

# Workload 2: Repeated reads (히트율 높음 - 정책 전환 유발)
echo "=== Workload 2: Repeated Reads (High Hit Rate) ==="
echo "Intensively reading SAME files repeatedly to trigger MRU..."
echo ""

# 작은 파일들만 반복적으로 읽기
FIRST_FILE=$(find "$WATCH_DIR/hot" -type f | head -1)

if [ -n "$FIRST_FILE" ]; then
    sudo cgexec -g memory:$CGROUP_NAME bash -c "
        echo 'Starting INTENSIVE repeated access (5000 times)...'
        for i in {1..5000}; do
            cat \"$FIRST_FILE\" > /dev/null 2>&1
        done
        echo 'Repeated access complete'
    "

    echo ""
    echo "✓ Repeated reads complete"
    echo "  Expected: High hit rate, policy should switch to MRU"
    echo ""
else
    echo "No files found to test"
fi

sleep 3

# Workload 3: Mixed pattern
echo "=== Workload 3: Mixed Pattern ==="
echo "HEAVY mixed pattern - alternating between sequential and repeated access..."
echo ""

sudo cgexec -g memory:$CGROUP_NAME bash -c "
    echo 'Starting heavy mixed workload (10 rounds)...'
    for round in {1..10}; do
        # Sequential scan (cold files)
        for file in $WATCH_DIR/cold/*.dat; do
            if [ -f \"\$file\" ]; then
                cat \"\$file\" > /dev/null 2>&1
            fi
        done

        # Repeated access (hot file)
        for i in {1..200}; do
            cat \"$FIRST_FILE\" > /dev/null 2>&1
        done
    done
    echo 'Heavy mixed workload complete'
"

echo ""
echo "✓ Mixed pattern complete"
echo "  Expected: Multiple policy switches (FIFO ↔ MRU)"
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
