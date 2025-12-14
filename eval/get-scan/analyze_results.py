#!/usr/bin/env python3
"""
Analyze GET-SCAN benchmark results
Compare adaptive_v2 with other policies
"""

import json
import sys
from pathlib import Path
from typing import Dict, List

def load_results(result_file: Path) -> List[Dict]:
    """Load benchmark results from JSON file"""
    try:
        with open(result_file) as f:
            return json.load(f)
    except FileNotFoundError:
        print(f"Warning: {result_file} not found", file=sys.stderr)
        return []
    except json.JSONDecodeError as e:
        print(f"Error parsing {result_file}: {e}", file=sys.stderr)
        return []

def calculate_stats(results: List[Dict], metric: str) -> Dict:
    """Calculate mean and std for a metric"""
    values = [r[metric] for r in results if metric in r]
    if not values:
        return {"mean": 0, "std": 0, "count": 0}
    
    mean = sum(values) / len(values)
    if len(values) > 1:
        variance = sum((x - mean) ** 2 for x in values) / (len(values) - 1)
        std = variance ** 0.5
    else:
        std = 0
    
    return {"mean": mean, "std": std, "count": len(values)}

def analyze_policy_switches(log_file: Path) -> Dict:
    """Extract policy switching information from log"""
    if not log_file.exists():
        return {"switch_count": 0, "switches": []}
    
    switches = []
    switch_count = 0
    
    with open(log_file) as f:
        lines = f.readlines()
    
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if "POLICY SWITCH DETECTED" in line:
            switch_count += 1
            switch_info = {"index": switch_count}
            
            # Extract details from following lines
            for j in range(i+1, min(i+20, len(lines))):
                detail_line = lines[j].strip()
                if "Old Policy:" in detail_line:
                    switch_info["from"] = detail_line.split(":")[-1].strip()
                elif "New Policy:" in detail_line:
                    switch_info["to"] = detail_line.split(":")[-1].strip()
                elif "Time:" in detail_line and "index" not in detail_line:
                    try:
                        switch_info["time"] = int(detail_line.split()[-1])
                    except:
                        pass
                elif "Hit Rate:" in detail_line:
                    try:
                        switch_info["hit_rate"] = detail_line.split()[-1]
                    except:
                        pass
                elif "Sequential Ratio:" in detail_line:
                    try:
                        switch_info["sequential_ratio"] = detail_line.split()[-1]
                    except:
                        pass
            
            switches.append(switch_info)
        i += 1
    
    return {"switch_count": switch_count, "switches": switches}

def main():
    # Find results directory
    script_dir = Path(__file__).parent
    results_dir = script_dir / "../../results"
    logs_dir = results_dir / "logs"
    
    policies = [
        "cache_ext_get_scan",
        "cache_ext_mru",
        "cache_ext_fifo",
        "cache_ext_lhd",
        "cache_ext_s3fifo",
        "cache_ext_adaptive_v2",
    ]
    
    print("=" * 70)
    print("GET-SCAN Benchmark Results Analysis")
    print("=" * 70)
    print()
    
    # Collect results for all policies
    all_results = {}
    for policy in policies:
        result_file = results_dir / f"get_scan_{policy}_results.json"
        results = load_results(result_file)
        
        if results:
            all_results[policy] = results
    
    if not all_results:
        print("No results found. Run the benchmark first.")
        return
    
    # Performance comparison
    print("Performance Comparison (READ operations)")
    print("-" * 70)
    print(f"{'Policy':<30} {'Throughput (ops/sec)':<25} {'Latency (ns)':<15}")
    print("-" * 70)
    
    best_throughput = 0
    best_policy = None
    
    for policy in policies:
        if policy not in all_results:
            print(f"{policy:<30} {'N/A':<25} {'N/A':<15}")
            continue
        
        throughput_stats = calculate_stats(all_results[policy], "read_throughput_avg")
        latency_stats = calculate_stats(all_results[policy], "read_latency_avg")
        
        if throughput_stats["count"] > 0:
            throughput_str = f"{throughput_stats['mean']:.2f} ± {throughput_stats['std']:.2f}"
            latency_str = f"{latency_stats['mean']:.0f} ± {latency_stats['std']:.0f}"
            
            if throughput_stats["mean"] > best_throughput:
                best_throughput = throughput_stats["mean"]
                best_policy = policy
        else:
            throughput_str = "N/A"
            latency_str = "N/A"
        
        print(f"{policy:<30} {throughput_str:<25} {latency_str:<15}")
    
    print("-" * 70)
    if best_policy:
        print(f"Best performance: {best_policy} ({best_throughput:.2f} ops/sec)")
    print()
    
    # SCAN operations comparison
    print("SCAN Operations Performance")
    print("-" * 70)
    print(f"{'Policy':<30} {'SCAN Throughput (ops/sec)':<30}")
    print("-" * 70)
    
    for policy in policies:
        if policy not in all_results:
            print(f"{policy:<30} {'N/A':<30}")
            continue
        
        scan_stats = calculate_stats(all_results[policy], "scan_throughput_avg")
        
        if scan_stats["count"] > 0 and scan_stats["mean"] > 0:
            scan_str = f"{scan_stats['mean']:.2f} ± {scan_stats['std']:.2f}"
        else:
            scan_str = "N/A"
        
        print(f"{policy:<30} {scan_str:<30}")
    
    print()
    
    # Adaptive_v2 specific analysis
    if "cache_ext_adaptive_v2" in all_results:
        print("=" * 70)
        print("Adaptive_v2 Policy Switching Analysis")
        print("=" * 70)
        
        log_file = logs_dir / "get_scan_cache_ext_adaptive_v2.log"
        switch_info = analyze_policy_switches(log_file)
        
        print(f"Total policy switches: {switch_info['switch_count']}")
        print()
        
        if switch_info['switch_count'] > 0:
            print("Policy switch timeline:")
            print(f"{'#':<5} {'From':<10} {'To':<10} {'Time':<15} {'Hit Rate':<12} {'Seq Ratio':<12}")
            print("-" * 70)
            
            for switch in switch_info['switches']:
                idx = str(switch.get('index', '?'))
                from_policy = str(switch.get('from', '?'))
                to_policy = str(switch.get('to', '?'))
                time = str(switch.get('time', '?'))
                hit_rate = str(switch.get('hit_rate', '?'))
                seq_ratio = str(switch.get('sequential_ratio', '?'))
                
                print(f"{idx:<5} {from_policy:<10} {to_policy:<10} {time:<15} {hit_rate:<12} {seq_ratio:<12}")
            
            print()
        else:
            print("No policy switches detected during benchmark.")
            print()
            print("Possible reasons:")
            print("  1. Thresholds too strict (check adaptive_v2.bpf.c)")
            print("  2. Workload didn't trigger switching conditions")
            print("  3. Policy switches not logged properly")
            print()
    
    # Relative performance
    if "cache_ext_adaptive_v2" in all_results and best_policy:
        print("=" * 70)
        print("Adaptive_v2 vs Best Policy")
        print("=" * 70)
        
        adaptive_throughput = calculate_stats(all_results["cache_ext_adaptive_v2"], "read_throughput_avg")["mean"]
        
        if best_policy != "cache_ext_adaptive_v2":
            improvement = ((adaptive_throughput - best_throughput) / best_throughput) * 100
            print(f"Adaptive_v2 throughput: {adaptive_throughput:.2f} ops/sec")
            print(f"Best ({best_policy}): {best_throughput:.2f} ops/sec")
            print(f"Difference: {improvement:+.2f}%")
        else:
            print("Adaptive_v2 achieved the best performance!")
        print()
    
    print("=" * 70)
    print("Analysis complete")
    print("=" * 70)

if __name__ == "__main__":
    main()
