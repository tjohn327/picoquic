#!/usr/bin/env python3
"""
Analyze deadline evaluation results from CSV file
"""

import csv
import sys
from datetime import datetime
from collections import defaultdict

def analyze_results(csv_file='deadline_evaluation_results.csv'):
    """Analyze and visualize deadline evaluation results"""
    
    # Read the CSV file
    try:
        df = pd.read_csv(csv_file)
        print(f"Loaded {len(df)} test results from {csv_file}")
    except FileNotFoundError:
        print(f"Error: {csv_file} not found")
        return
    
    # Convert timestamp to datetime
    df['datetime'] = pd.to_datetime(df['timestamp'], unit='s')
    
    # Basic statistics
    print("\n=== EVALUATION SUMMARY ===")
    print(f"Total tests: {len(df)}")
    print(f"Test period: {df['datetime'].min()} to {df['datetime'].max()}")
    
    # Group by test configuration
    configs = df.groupby(['deadline_enabled', 'multipath_enabled'])
    
    print("\n=== PERFORMANCE BY CONFIGURATION ===")
    for (deadline, multipath), group in configs:
        config_name = f"{'Deadline' if deadline else 'Vanilla'} {'Multipath' if multipath else 'Single-path'}"
        print(f"\n{config_name}:")
        print(f"  Tests: {len(group)}")
        print(f"  Avg Duration: {group['duration_sec'].mean():.2f}s")
        print(f"  Avg Throughput: {group['throughput_mbps'].mean():.2f} Mbps")
        if deadline:
            print(f"  Avg Compliance: {group['compliance_pct'].mean():.1f}%")
            print(f"  Avg Latency: {group['avg_latency_ms'].mean():.1f}ms")
    
    # Calculate improvements
    vanilla_metrics = df[df['deadline_enabled'] == 0].groupby('test_name')[['duration_sec', 'throughput_mbps']].mean()
    deadline_metrics = df[df['deadline_enabled'] == 1].groupby('test_name')[['duration_sec', 'throughput_mbps']].mean()
    
    print("\n=== DEADLINE vs VANILLA IMPROVEMENTS ===")
    for test in vanilla_metrics.index:
        if test.replace('Vanilla_', 'Deadline_') in deadline_metrics.index:
            vanilla = vanilla_metrics.loc[test]
            deadline_test = test.replace('Vanilla_', 'Deadline_')
            if deadline_test in deadline_metrics.index:
                deadline = deadline_metrics.loc[deadline_test]
                duration_improvement = (vanilla['duration_sec'] - deadline['duration_sec']) / vanilla['duration_sec'] * 100
                throughput_improvement = (deadline['throughput_mbps'] - vanilla['throughput_mbps']) / vanilla['throughput_mbps'] * 100
                
                print(f"\n{test}:")
                print(f"  Duration: {vanilla['duration_sec']:.2f}s → {deadline['duration_sec']:.2f}s ({duration_improvement:.1f}% improvement)")
                print(f"  Throughput: {vanilla['throughput_mbps']:.2f} → {deadline['throughput_mbps']:.2f} Mbps ({throughput_improvement:.1f}% improvement)")
    
    # Create visualizations if we have enough data
    if len(df) >= 2:
        plt.figure(figsize=(12, 8))
        
        # Plot 1: Duration comparison
        plt.subplot(2, 2, 1)
        duration_data = df.pivot_table(values='duration_sec', 
                                       index='test_name', 
                                       columns='deadline_enabled', 
                                       aggfunc='mean')
        duration_data.plot(kind='bar')
        plt.title('Test Duration Comparison')
        plt.ylabel('Duration (seconds)')
        plt.xlabel('Test Scenario')
        plt.legend(['Vanilla QUIC', 'Deadline QUIC'])
        plt.xticks(rotation=45, ha='right')
        
        # Plot 2: Throughput comparison
        plt.subplot(2, 2, 2)
        throughput_data = df.pivot_table(values='throughput_mbps', 
                                        index='test_name', 
                                        columns='deadline_enabled', 
                                        aggfunc='mean')
        throughput_data.plot(kind='bar')
        plt.title('Throughput Comparison')
        plt.ylabel('Throughput (Mbps)')
        plt.xlabel('Test Scenario')
        plt.legend(['Vanilla QUIC', 'Deadline QUIC'])
        plt.xticks(rotation=45, ha='right')
        
        # Plot 3: Deadline compliance (if available)
        deadline_df = df[df['deadline_enabled'] == 1]
        if len(deadline_df) > 0:
            plt.subplot(2, 2, 3)
            deadline_df.groupby('test_name')['compliance_pct'].mean().plot(kind='bar', color='green')
            plt.title('Deadline Compliance by Test')
            plt.ylabel('Compliance (%)')
            plt.xlabel('Test Scenario')
            plt.axhline(y=95, color='r', linestyle='--', label='95% Target')
            plt.ylim(0, 105)
            plt.xticks(rotation=45, ha='right')
            plt.legend()
            
            # Plot 4: Average latency
            plt.subplot(2, 2, 4)
            deadline_df.groupby('test_name')['avg_latency_ms'].mean().plot(kind='bar', color='orange')
            plt.title('Average Latency by Test')
            plt.ylabel('Latency (ms)')
            plt.xlabel('Test Scenario')
            plt.xticks(rotation=45, ha='right')
        
        plt.tight_layout()
        plt.savefig('deadline_evaluation_analysis.png', dpi=300, bbox_inches='tight')
        print("\nVisualization saved to: deadline_evaluation_analysis.png")
    
    # Export summary statistics
    summary = df.groupby(['test_name', 'deadline_enabled', 'multipath_enabled']).agg({
        'duration_sec': ['mean', 'std'],
        'throughput_mbps': ['mean', 'std'],
        'compliance_pct': 'mean',
        'avg_latency_ms': 'mean'
    }).round(2)
    
    summary.to_csv('deadline_evaluation_summary.csv')
    print("\nSummary statistics saved to: deadline_evaluation_summary.csv")
    
    # Key findings
    print("\n=== KEY FINDINGS ===")
    
    # Find best performing configuration
    best_throughput = df.loc[df['throughput_mbps'].idxmax()]
    print(f"\nBest throughput: {best_throughput['test_name']} - {best_throughput['throughput_mbps']:.2f} Mbps")
    print(f"  Configuration: {'Deadline' if best_throughput['deadline_enabled'] else 'Vanilla'} "
          f"{'Multipath' if best_throughput['multipath_enabled'] else 'Single-path'}")
    
    # Check deadline compliance
    deadline_tests = df[df['deadline_enabled'] == 1]
    if len(deadline_tests) > 0:
        avg_compliance = deadline_tests['compliance_pct'].mean()
        print(f"\nAverage deadline compliance: {avg_compliance:.1f}%")
        
        poor_compliance = deadline_tests[deadline_tests['compliance_pct'] < 95]
        if len(poor_compliance) > 0:
            print(f"\nTests with poor compliance (<95%):")
            for _, test in poor_compliance.iterrows():
                print(f"  - {test['test_name']}: {test['compliance_pct']:.1f}%")
        else:
            print("\nAll deadline tests achieved >95% compliance!")
    
    return df

if __name__ == "__main__":
    csv_file = sys.argv[1] if len(sys.argv) > 1 else 'deadline_evaluation_results.csv'
    analyze_results(csv_file)