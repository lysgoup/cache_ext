#!/bin/bash
# GET-SCAN Comprehensive Policy Comparison
# Compares adaptive_v2 with baseline policies: get_scan, MRU, FIFO, LHD, S3-FIFO
set -eu -o pipefail

if ! uname -r | grep -q "cache-ext"; then
	echo "ERROR: This script requires cache_ext kernel."
	echo "Current kernel: $(uname -r)"
	exit 1
fi

SCRIPT_PATH=$(realpath $0)
BASE_DIR=$(realpath "$(dirname $SCRIPT_PATH)/../../")
BENCH_PATH="$BASE_DIR/bench"
POLICY_PATH="$BASE_DIR/policies"
YCSB_PATH="$BASE_DIR/My-YCSB"
DB_PATH=$(realpath "$BASE_DIR/../leveldb")
RESULTS_PATH="$BASE_DIR/results"
LOGS_PATH="$RESULTS_PATH/logs"

ITERATIONS=3

# Policies to compare
POLICIES=(
	"cache_ext_get_scan"
	"cache_ext_mru"
	"cache_ext_fifo"
	"cache_ext_lhd"
	"cache_ext_s3fifo"
	"cache_ext_adaptive_v2"
)

echo "========================================="
echo "GET-SCAN Policy Comparison Benchmark"
echo "========================================="
echo "Base dir:    $BASE_DIR"
echo "DB path:     $DB_PATH"
echo "Results:     $RESULTS_PATH"
echo "Iterations:  $ITERATIONS"
echo ""
echo "Policies to test:"
for policy in "${POLICIES[@]}"; do
	echo "  - $policy"
done
echo ""

# Pre-flight checks
echo "[Pre-flight checks]"

# Check if all policies exist
MISSING_POLICIES=()
for policy in "${POLICIES[@]}"; do
	if [ ! -f "$POLICY_PATH/${policy}.out" ]; then
		MISSING_POLICIES+=("$policy")
	fi
done

if [ ${#MISSING_POLICIES[@]} -gt 0 ]; then
	echo "WARNING: Following policies not found:"
	for policy in "${MISSING_POLICIES[@]}"; do
		echo "  ✗ ${policy}.out"
	done
	echo ""
	echo "Build missing policies with:"
	echo "  cd $POLICY_PATH"
	for policy in "${MISSING_POLICIES[@]}"; do
		echo "  make ${policy}.out"
	done
	echo ""
	read -p "Continue without these policies? (y/n) " -n 1 -r
	echo ""
	if [[ ! $REPLY =~ ^[Yy]$ ]]; then
		exit 1
	fi
	# Remove missing policies from list
	for missing in "${MISSING_POLICIES[@]}"; do
		POLICIES=("${POLICIES[@]/$missing}")
	done
fi

# Check if LevelDB exists
if [ ! -d "$DB_PATH" ]; then
	echo "ERROR: LevelDB database not found at $DB_PATH"
	exit 1
fi
echo "✓ LevelDB found ($(du -sh $DB_PATH | cut -f1))"

# Check if My-YCSB exists
if [ ! -d "$YCSB_PATH" ]; then
	echo "ERROR: My-YCSB not found at $YCSB_PATH"
	exit 1
fi
echo "✓ My-YCSB found"
echo ""

mkdir -p "$RESULTS_PATH"
mkdir -p "$LOGS_PATH"

# Build My-YCSB with leveldb-scan branch
echo "[1/3] Building My-YCSB (leveldb-scan branch)..."
cd "$YCSB_PATH/build"
git checkout leveldb-scan 2>&1 | head -5
make clean > /dev/null 2>&1
make -j run_leveldb
cd -
echo "✓ Build complete"
echo ""

# Disable MGLRU
echo "[2/3] Disabling MGLRU..."
if ! "$BASE_DIR/utils/disable-mglru.sh"; then
	echo "ERROR: Failed to disable MGLRU"
	exit 1
fi
echo "✓ MGLRU disabled"
echo ""

# Run benchmarks
echo "[3/3] Running benchmarks..."
echo "Estimated total time: $((${#POLICIES[@]} * ITERATIONS * 5)) minutes"
echo ""

POLICY_COUNT=0
TOTAL_POLICIES=${#POLICIES[@]}

for policy in "${POLICIES[@]}"; do
	# Skip if empty (removed earlier)
	if [ -z "$policy" ]; then
		continue
	fi
	
	POLICY_COUNT=$((POLICY_COUNT + 1))
	
	echo "========================================="
	echo "[$POLICY_COUNT/$TOTAL_POLICIES] Testing: $policy"
	echo "========================================="
	echo "Started: $(date)"
	echo ""
	
	RESULT_FILE="$RESULTS_PATH/get_scan_${policy}_results.json"
	LOG_FILE="$LOGS_PATH/get_scan_${policy}.log"
	
	# Run benchmark
	if python3 "$BENCH_PATH/bench_leveldb.py" \
		--cpu 8 \
		--policy-loader "$POLICY_PATH/${policy}.out" \
		--results-file "$RESULT_FILE" \
		--leveldb-db "$DB_PATH" \
		--fadvise-hints "" \
		--iterations "$ITERATIONS" \
		--bench-binary-dir "$YCSB_PATH/build" \
		--benchmark mixed_get_scan 2>&1 | tee "$LOG_FILE"; then
		
		echo ""
		echo "✓ $policy complete"
		
		# For adaptive_v2, check for policy switches
		if [[ "$policy" == "cache_ext_adaptive_v2" ]]; then
			SWITCH_COUNT=$(grep -c "POLICY SWITCH DETECTED" "$LOG_FILE" 2>/dev/null || echo "0")
			echo "  Policy switches detected: $SWITCH_COUNT"
			if [ "$SWITCH_COUNT" -gt 0 ]; then
				echo "  Details logged in: $LOG_FILE"
			fi
		fi
		
		echo ""
	else
		echo ""
		echo "✗ $policy FAILED"
		echo "  Check logs: $LOG_FILE"
		echo ""
	fi
	
	# Brief pause between policies
	sleep 5
done

# Cleanup
echo "========================================="
echo "Cleanup"
echo "========================================="
if ! "$BASE_DIR/utils/disable-mglru.sh"; then
	echo "Warning: Failed to disable MGLRU during cleanup"
fi
echo "✓ Cleanup complete"
echo ""

# Results summary
echo "========================================="
echo "Benchmark Complete!"
echo "========================================="
echo "Completed: $(date)"
echo ""
echo "Results saved to: $RESULTS_PATH"
echo ""

# Show throughput comparison
echo "Performance Summary (READ throughput):"
echo "--------------------------------------"
for policy in "${POLICIES[@]}"; do
	if [ -z "$policy" ]; then
		continue
	fi
	
	RESULT_FILE="$RESULTS_PATH/get_scan_${policy}_results.json"
	if [ -f "$RESULT_FILE" ]; then
		# Extract average READ throughput
		AVG_THROUGHPUT=$(python3 -c "
import json
import sys
try:
    with open('$RESULT_FILE') as f:
        data = json.load(f)
    throughputs = [r['read_throughput_avg'] for r in data if 'read_throughput_avg' in r]
    if throughputs:
        print(f'{sum(throughputs)/len(throughputs):.2f}')
    else:
        print('N/A')
except:
    print('ERROR')
" 2>/dev/null || echo "N/A")
		
		printf "%-25s %15s ops/sec\n" "$policy:" "$AVG_THROUGHPUT"
	else
		printf "%-25s %15s\n" "$policy:" "FAILED"
	fi
done
echo ""

echo "Detailed results:"
for policy in "${POLICIES[@]}"; do
	if [ -z "$policy" ]; then
		continue
	fi
	echo "  $RESULTS_PATH/get_scan_${policy}_results.json"
done
echo ""

echo "Logs:"
for policy in "${POLICIES[@]}"; do
	if [ -z "$policy" ]; then
		continue
	fi
	echo "  $LOGS_PATH/get_scan_${policy}.log"
done
echo ""

# Special adaptive_v2 analysis
ADAPTIVE_LOG="$LOGS_PATH/get_scan_cache_ext_adaptive_v2.log"
if [ -f "$ADAPTIVE_LOG" ]; then
	echo "========================================="
	echo "Adaptive_v2 Policy Switching Analysis"
	echo "========================================="
	
	SWITCH_COUNT=$(grep -c "POLICY SWITCH DETECTED" "$ADAPTIVE_LOG" 2>/dev/null || echo "0")
	echo "Total policy switches: $SWITCH_COUNT"
	echo ""
	
	if [ "$SWITCH_COUNT" -gt 0 ]; then
		echo "Policy switch timeline:"
		grep -B 2 "POLICY SWITCH DETECTED" "$ADAPTIVE_LOG" | \
			grep -E "Old Policy|New Policy" | \
			head -20
	else
		echo "No policy switches detected."
		echo ""
		echo "Possible reasons:"
		echo "  1. Thresholds too strict (MIN_SAMPLES=1000, MIN_TIME_IN_POLICY=10000)"
		echo "  2. Workload didn't vary enough to trigger switches"
		echo "  3. Policy switches not being logged properly"
		echo ""
		echo "Check the full log for details:"
		echo "  $ADAPTIVE_LOG"
	fi
fi

echo ""
echo "========================================="
echo "All benchmarks completed successfully!"
echo "========================================="
