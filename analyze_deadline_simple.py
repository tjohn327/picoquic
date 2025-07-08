#!/usr/bin/env python3
"""
Simple analysis of deadline evaluation results
"""

import csv
import sys
from datetime import datetime
from collections import defaultdict

def analyze_results(csv_file='deadline_evaluation_results.csv'):
    """Analyze deadline evaluation results"""
    
    # Read the CSV file
    try:
        with open(csv_file, 'r') as f:
            reader = csv.DictReader(f)
            results = list(reader)
        print(f"Loaded {len(results)} test results from {csv_file}")
    except FileNotFoundError:
        print(f"Error: {csv_file} not found")
        return
    
    # Convert numeric fields
    for r in results:
        r['deadline_enabled'] = int(r['deadline_enabled'])
        r['multipath_enabled'] = int(r['multipath_enabled'])
        r['num_streams'] = int(r['num_streams'])
        r['duration_sec'] = float(r['duration_sec'])
        r['throughput_mbps'] = float(r['throughput_mbps'])
        r['compliance_pct'] = float(r['compliance_pct'])
        r['avg_latency_ms'] = float(r['avg_latency_ms'])
        r['datetime'] = datetime.fromtimestamp(int(r['timestamp']))
    
    # Basic statistics
    print("\n=== EVALUATION SUMMARY ===")
    print(f"Total tests: {len(results)}")
    if results:
        print(f"Test period: {results[0]['datetime']} to {results[-1]['datetime']}")
    
    # Group by configuration
    configs = defaultdict(list)
    for r in results:
        key = (r['deadline_enabled'], r['multipath_enabled'])
        configs[key].append(r)
    
    print("\n=== PERFORMANCE BY CONFIGURATION ===")
    for (deadline, multipath), tests in configs.items():
        config_name = f"{'Deadline' if deadline else 'Vanilla'} {'Multipath' if multipath else 'Single-path'}"
        print(f"\n{config_name}:")
        print(f"  Tests: {len(tests)}")
        
        avg_duration = sum(t['duration_sec'] for t in tests) / len(tests)
        avg_throughput = sum(t['throughput_mbps'] for t in tests) / len(tests)
        print(f"  Avg Duration: {avg_duration:.2f}s")
        print(f"  Avg Throughput: {avg_throughput:.2f} Mbps")
        
        if deadline:
            avg_compliance = sum(t['compliance_pct'] for t in tests) / len(tests)
            avg_latency = sum(t['avg_latency_ms'] for t in tests) / len(tests)
            print(f"  Avg Compliance: {avg_compliance:.1f}%")
            print(f"  Avg Latency: {avg_latency:.1f}ms")
    
    # Calculate improvements
    vanilla_tests = {}
    deadline_tests = {}
    
    for r in results:
        if r['deadline_enabled'] == 0:
            vanilla_tests[r['test_name']] = r
        else:
            deadline_tests[r['test_name']] = r
    
    print("\n=== DEADLINE vs VANILLA IMPROVEMENTS ===")
    for test_name, vanilla in vanilla_tests.items():
        # Look for corresponding deadline test
        deadline_name = test_name.replace('Vanilla_', 'Deadline_')
        if deadline_name in deadline_tests:
            deadline = deadline_tests[deadline_name]
            
            duration_improvement = (vanilla['duration_sec'] - deadline['duration_sec']) / vanilla['duration_sec'] * 100
            throughput_improvement = (deadline['throughput_mbps'] - vanilla['throughput_mbps']) / vanilla['throughput_mbps'] * 100 if vanilla['throughput_mbps'] > 0 else 0
            
            print(f"\n{test_name}:")
            print(f"  Duration: {vanilla['duration_sec']:.2f}s → {deadline['duration_sec']:.2f}s ({duration_improvement:.1f}% faster)")
            print(f"  Throughput: {vanilla['throughput_mbps']:.2f} → {deadline['throughput_mbps']:.2f} Mbps ({throughput_improvement:.1f}% improvement)")
    
    # Key findings
    print("\n=== KEY FINDINGS ===")
    
    if results:
        # Best throughput
        best_throughput = max(results, key=lambda x: x['throughput_mbps'])
        print(f"\nBest throughput: {best_throughput['test_name']} - {best_throughput['throughput_mbps']:.2f} Mbps")
        print(f"  Configuration: {'Deadline' if best_throughput['deadline_enabled'] else 'Vanilla'} "
              f"{'Multipath' if best_throughput['multipath_enabled'] else 'Single-path'}")
        
        # Fastest completion
        fastest = min(results, key=lambda x: x['duration_sec'])
        print(f"\nFastest completion: {fastest['test_name']} - {fastest['duration_sec']:.2f}s")
        
        # Check deadline compliance
        deadline_results = [r for r in results if r['deadline_enabled'] == 1]
        if deadline_results:
            avg_compliance = sum(r['compliance_pct'] for r in deadline_results) / len(deadline_results)
            print(f"\nAverage deadline compliance: {avg_compliance:.1f}%")
            
            poor_compliance = [r for r in deadline_results if r['compliance_pct'] < 95]
            if poor_compliance:
                print(f"\nTests with poor compliance (<95%):")
                for test in poor_compliance:
                    print(f"  - {test['test_name']}: {test['compliance_pct']:.1f}%")
            else:
                print("\nAll deadline tests achieved >95% compliance!")
    
    # Write summary
    with open('deadline_evaluation_summary.txt', 'w') as f:
        f.write("DEADLINE EVALUATION SUMMARY\n")
        f.write("===========================\n\n")
        
        for (deadline, multipath), tests in configs.items():
            config_name = f"{'Deadline' if deadline else 'Vanilla'} {'Multipath' if multipath else 'Single-path'}"
            f.write(f"{config_name}:\n")
            
            for test in tests:
                f.write(f"  {test['test_name']}: {test['duration_sec']:.2f}s, {test['throughput_mbps']:.2f} Mbps")
                if deadline:
                    f.write(f", {test['compliance_pct']:.1f}% compliance, {test['avg_latency_ms']:.1f}ms latency")
                f.write("\n")
            f.write("\n")
    
    print("\nSummary written to: deadline_evaluation_summary.txt")

if __name__ == "__main__":
    csv_file = sys.argv[1] if len(sys.argv) > 1 else 'deadline_evaluation_results.csv'
    analyze_results(csv_file)