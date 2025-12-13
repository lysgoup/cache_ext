#!/bin/bash

# Workload 테스트 스크립트 v3 - adaptive policy v3의 정책 전환을 유도

CGROUP_PATH="/sys/fs/cgroup/adaptive_v3_test"
WATCH_DIR="/mydata/leveldb_db"

echo "=================================="
echo "Adaptive Policy v3 Workload Test"
echo "=================================="
echo ""
echo "This script runs different workloads to trigger policy switches in v3"
echo "Expected policy switches:"
echo "  • Sequential scan → FIFO"
echo "  • Hot working set → MRU"
echo "  • Mixed workload → S3-FIFO or LRU"
echo ""

# Cgroup 확인
if [ ! -d "$CGROUP_PATH" ]; then
    echo "ERROR: Cgroup $CGROUP_PATH does not exist"
    echo "Run test_adaptive_v3.sh first!"
    exit 1
fi

# 테스트 디렉토리 확인 및 생성
if [ ! -d "$WATCH_DIR" ]; then
    echo "Warning: $WATCH_DIR does not exist"

    # 대체 디렉토리 설정
    if [ -d "/mydata" ]; then
        WATCH_DIR="/mydata/adaptive_test"
    else
        WATCH_DIR="/tmp/adaptive_test"
    fi

    mkdir -p "$WATCH_DIR"

    # 테스트 파일 생성
    echo "Creating test files in $WATCH_DIR..."
    for i in {1..50}; do
        dd if=/dev/urandom of="$WATCH_DIR/file_$i.dat" bs=1M count=5 2>/dev/null
    done
    echo "✓ Created 50 test files (5MB each)"
    echo ""
fi

echo "Watch directory: $WATCH_DIR"
echo "Cgroup: $CGROUP_PATH"
echo ""
echo "Make sure cache_ext_adaptive_v3 is running in another terminal!"
echo ""
read -p "Press Enter to start workload tests..."
echo ""

# Workload 1: Sequential Scan (FIFO 유도)
echo "=========================================="
echo "Workload 1: Sequential Scan"
echo "=========================================="
echo "Goal: Trigger FIFO policy"
echo "  • High one-time ratio"
echo "  • High sequential ratio"
echo "  • Low avg hits per page"
echo ""

sudo cgexec -g memory:adaptive_v3_test bash -c "
    echo 'Reading all files sequentially (once)...'
    for file in $WATCH_DIR/*.dat $WATCH_DIR/*; do
        if [ -f \"\$file\" ]; then
            cat \"\$file\" > /dev/null 2>&1
        fi
    done
"

echo ""
echo "✓ Sequential scan complete"
echo "  Expected: Policy → FIFO"
echo ""
sleep 5

# Workload 2: Hot Working Set (MRU 유도)
echo "=========================================="
echo "Workload 2: Hot Working Set (Small)"
echo "=========================================="
echo "Goal: Trigger MRU policy"
echo "  • Low one-time ratio"
echo "  • High avg hits per page"
echo "  • Working set << cache"
echo ""

# 작은 파일들을 반복 접근
SMALL_FILES=$(find "$WATCH_DIR" -type f | head -5)

sudo cgexec -g memory:adaptive_v3_test bash -c "
    echo 'Repeatedly accessing small hot set...'
    for round in {1..20}; do
        for file in $SMALL_FILES; do
            cat \"\$file\" > /dev/null 2>&1
        done
    done
"

echo ""
echo "✓ Hot working set test complete"
echo "  Expected: Policy → MRU"
echo "  Working set size: ~5 files"
echo ""
sleep 5

# Workload 3: Large Working Set (FIFO 유도)
echo "=========================================="
echo "Workload 3: Large Working Set"
echo "=========================================="
echo "Goal: Working set >> cache"
echo "  • High working set ratio"
echo "  • Policy → FIFO"
echo ""

sudo cgexec -g memory:adaptive_v3_test bash -c "
    echo 'Accessing many different files...'
    for round in {1..3}; do
        for file in $WATCH_DIR/*.dat; do
            if [ -f \"\$file\" ]; then
                cat \"\$file\" > /dev/null 2>&1
            fi
        done
    done
"

echo ""
echo "✓ Large working set test complete"
echo "  Expected: Working set >> cache → FIFO"
echo ""
sleep 5

# Workload 4: Mixed Hot/Cold (S3-FIFO 유도)
echo "=========================================="
echo "Workload 4: Mixed Hot/Cold Pattern"
echo "=========================================="
echo "Goal: Trigger S3-FIFO policy"
echo "  • Mixed one-time ratio (40-60%)"
echo "  • Hot + cold pages"
echo ""

HOT_FILE=$(find "$WATCH_DIR" -type f | head -1)
ALL_FILES=$(find "$WATCH_DIR" -type f)

sudo cgexec -g memory:adaptive_v3_test bash -c "
    echo 'Mixing hot (repeated) and cold (one-time) access...'
    for round in {1..10}; do
        # Hot access
        for i in {1..10}; do
            cat \"$HOT_FILE\" > /dev/null 2>&1
        done

        # Cold access (random files)
        for file in \$(echo \"$ALL_FILES\" | shuf | head -5); do
            cat \"\$file\" > /dev/null 2>&1
        done
    done
"

echo ""
echo "✓ Mixed pattern complete"
echo "  Expected: Policy → S3-FIFO (hot/cold separation)"
echo ""
sleep 5

# Workload 5: Random Access (LRU 유도)
echo "=========================================="
echo "Workload 5: Random Access Pattern"
echo "=========================================="
echo "Goal: Trigger LRU policy"
echo "  • Low sequential ratio"
echo "  • Medium reuse distance"
echo ""

sudo cgexec -g memory:adaptive_v3_test bash -c "
    echo 'Random file access...'
    for i in {1..100}; do
        RANDOM_FILE=\$(find $WATCH_DIR -type f | shuf | head -1)
        cat \"\$RANDOM_FILE\" > /dev/null 2>&1
    done
"

echo ""
echo "✓ Random access complete"
echo "  Expected: Policy → LRU (balanced)"
echo ""

# 정리
if [ "$WATCH_DIR" = "/tmp/adaptive_test" ]; then
    echo ""
    read -p "Clean up test files? (y/n) " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Cleaning up test files..."
        rm -rf "$WATCH_DIR"
        echo "✓ Cleanup complete"
    fi
fi

echo ""
echo "=========================================="
echo "All workload tests complete!"
echo "=========================================="
echo ""
echo "Summary of expected policy switches:"
echo "  1. Sequential scan     → FIFO"
echo "  2. Hot working set     → MRU"
echo "  3. Large working set   → FIFO"
echo "  4. Mixed hot/cold      → S3-FIFO"
echo "  5. Random access       → LRU"
echo ""
echo "Check the v3 output for:"
echo "  • Policy switch events"
echo "  • Working set size changes"
echo "  • Metric values (one-time ratio, sequential ratio, etc.)"
echo ""
