#!/usr/bin/env python3
"""
Analyze DMTP Multipath Evaluation Results

This script reads the comprehensive_multipath_eval_results.csv file and generates
various graphs to visualize the performance comparison between vanilla and 
deadline-aware multipath QUIC.
"""

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np
from pathlib import Path
import sys

# Set style for better-looking plots
plt.style.use('seaborn-v0_8-darkgrid')
sns.set_palette("husl")

def load_data(csv_path):
    """Load and preprocess the evaluation results."""
    try:
        df = pd.read_csv(csv_path)
        print(f"Loaded {len(df)} test results from {csv_path}")
        
        # Add a mode column for easier grouping
        df['mode'] = df['deadline_enabled'].map({0: 'Vanilla', 1: 'Deadline-aware'})
        
        # Calculate path utilization percentages
        df['path0_utilization'] = (df['path0_bytes'] / df['total_bytes'] * 100).round(1)
        df['path1_utilization'] = (df['path1_bytes'] / df['total_bytes'] * 100).round(1)
        
        return df
    except Exception as e:
        print(f"Error loading data: {e}")
        sys.exit(1)

def plot_compliance_comparison(df, output_dir):
    """Plot deadline compliance comparison across scenarios and networks."""
    fig, axes = plt.subplots(2, 2, figsize=(15, 12))
    fig.suptitle('Deadline Compliance Comparison: Vanilla vs Deadline-aware', fontsize=16)
    
    scenarios = df['scenario'].unique()
    
    for idx, scenario in enumerate(scenarios[:4]):  # Plot first 4 scenarios
        ax = axes[idx // 2, idx % 2]
        scenario_df = df[df['scenario'] == scenario]
        
        # Group by network and mode
        pivot_df = scenario_df.pivot_table(
            values='compliance_pct', 
            index='network', 
            columns='mode',
            aggfunc='mean'
        )
        
        pivot_df.plot(kind='bar', ax=ax, width=0.8)
        ax.set_title(f'{scenario.replace("_", " ").title()}', fontsize=14)
        ax.set_xlabel('Network Configuration')
        ax.set_ylabel('Compliance (%)')
        ax.set_ylim(0, 105)
        ax.legend(title='Mode', loc='lower right')
        ax.grid(axis='y', alpha=0.3)
        
        # Add value labels on bars
        for container in ax.containers:
            ax.bar_label(container, fmt='%.1f%%', padding=3)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'compliance_comparison.png', dpi=300, bbox_inches='tight')
    print("Saved: compliance_comparison.png")

def plot_latency_comparison(df, output_dir):
    """Plot average latency comparison."""
    plt.figure(figsize=(12, 8))
    
    # Filter out zero latencies (likely measurement issues)
    df_filtered = df[df['avg_latency_ms'] > 0]
    
    # Create box plot
    ax = sns.boxplot(data=df_filtered, x='scenario', y='avg_latency_ms', 
                     hue='mode', palette=['lightcoral', 'lightblue'])
    
    plt.title('Average Latency Distribution by Scenario', fontsize=16)
    plt.xlabel('Scenario', fontsize=12)
    plt.ylabel('Average Latency (ms)', fontsize=12)
    plt.xticks(rotation=45, ha='right')
    
    # Use log scale for better visualization of differences
    plt.yscale('log')
    plt.grid(axis='y', alpha=0.3)
    
    # Add median values as text
    medians = df_filtered.groupby(['scenario', 'mode'])['avg_latency_ms'].median()
    for i, scenario in enumerate(df_filtered['scenario'].unique()):
        for j, mode in enumerate(['Vanilla', 'Deadline-aware']):
            if (scenario, mode) in medians:
                median_val = medians[(scenario, mode)]
                offset = 0.2 if j == 0 else -0.2
                plt.text(i + offset, median_val, f'{median_val:.1f}', 
                        ha='center', va='bottom', fontsize=9)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'latency_comparison.png', dpi=300, bbox_inches='tight')
    print("Saved: latency_comparison.png")

def plot_network_impact(df, output_dir):
    """Plot the impact of different network configurations."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))
    
    # Compliance by network
    network_compliance = df.groupby(['network', 'mode'])['compliance_pct'].mean().reset_index()
    pivot_compliance = network_compliance.pivot(index='network', columns='mode', values='compliance_pct')
    
    pivot_compliance.plot(kind='bar', ax=ax1, width=0.8)
    ax1.set_title('Average Compliance by Network Type', fontsize=14)
    ax1.set_xlabel('Network Configuration')
    ax1.set_ylabel('Average Compliance (%)')
    ax1.set_ylim(0, 105)
    ax1.legend(title='Mode')
    ax1.grid(axis='y', alpha=0.3)
    
    # Add value labels
    for container in ax1.containers:
        ax1.bar_label(container, fmt='%.1f%%', padding=3)
    
    # Throughput by network
    network_throughput = df.groupby(['network', 'mode'])['throughput_mbps'].mean().reset_index()
    pivot_throughput = network_throughput.pivot(index='network', columns='mode', values='throughput_mbps')
    
    pivot_throughput.plot(kind='bar', ax=ax2, width=0.8)
    ax2.set_title('Average Throughput by Network Type', fontsize=14)
    ax2.set_xlabel('Network Configuration')
    ax2.set_ylabel('Throughput (Mbps)')
    ax2.legend(title='Mode')
    ax2.grid(axis='y', alpha=0.3)
    
    # Add value labels
    for container in ax2.containers:
        ax2.bar_label(container, fmt='%.2f', padding=3)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'network_impact.png', dpi=300, bbox_inches='tight')
    print("Saved: network_impact.png")

def plot_path_utilization(df, output_dir):
    """Plot multipath utilization patterns."""
    fig, axes = plt.subplots(2, 2, figsize=(15, 12))
    fig.suptitle('Path Utilization Patterns', fontsize=16)
    
    networks = df['network'].unique()
    
    for idx, network in enumerate(networks):
        ax = axes[idx // 2, idx % 2]
        network_df = df[df['network'] == network]
        
        # Calculate mean path utilization by scenario and mode
        util_data = []
        for scenario in network_df['scenario'].unique():
            for mode in ['Vanilla', 'Deadline-aware']:
                scenario_mode_df = network_df[(network_df['scenario'] == scenario) & 
                                             (network_df['mode'] == mode)]
                if not scenario_mode_df.empty:
                    util_data.append({
                        'scenario': scenario,
                        'mode': mode,
                        'Path 0': scenario_mode_df['path0_utilization'].mean(),
                        'Path 1': scenario_mode_df['path1_utilization'].mean()
                    })
        
        if util_data:
            util_df = pd.DataFrame(util_data)
            
            # Create grouped bar chart
            x = np.arange(len(util_df))
            width = 0.35
            
            ax.bar(x - width/2, util_df['Path 0'], width, label='Path 0', alpha=0.8)
            ax.bar(x + width/2, util_df['Path 1'], width, label='Path 1', alpha=0.8)
            
            ax.set_title(f'{network.replace("_", " ")}', fontsize=14)
            ax.set_xlabel('Scenario - Mode')
            ax.set_ylabel('Path Utilization (%)')
            ax.set_ylim(0, 105)
            ax.set_xticks(x)
            ax.set_xticklabels([f"{row['scenario'].split('_')[0][:4]}\n{row['mode'][:3]}" 
                               for _, row in util_df.iterrows()], 
                              rotation=45, ha='right', fontsize=8)
            ax.legend()
            ax.grid(axis='y', alpha=0.3)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'path_utilization.png', dpi=300, bbox_inches='tight')
    print("Saved: path_utilization.png")

def plot_deadline_effectiveness(df, output_dir):
    """Plot deadline effectiveness metrics."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))
    
    # Filter for tests with deadline streams
    deadline_df = df[df['deadline_streams'] > 0]
    
    # On-time delivery rate
    deadline_df['on_time_rate'] = (deadline_df['on_time_chunks'] / 
                                   deadline_df['total_chunks'] * 100)
    
    # Plot 1: On-time delivery rate by scenario
    scenario_rates = deadline_df.groupby(['scenario', 'mode'])['on_time_rate'].mean().reset_index()
    pivot_rates = scenario_rates.pivot(index='scenario', columns='mode', values='on_time_rate')
    
    pivot_rates.plot(kind='bar', ax=ax1, width=0.8)
    ax1.set_title('On-time Chunk Delivery Rate by Scenario', fontsize=14)
    ax1.set_xlabel('Scenario')
    ax1.set_ylabel('On-time Delivery Rate (%)')
    ax1.set_ylim(0, 105)
    ax1.legend(title='Mode')
    ax1.grid(axis='y', alpha=0.3)
    
    # Add value labels
    for container in ax1.containers:
        ax1.bar_label(container, fmt='%.1f%%', padding=3)
    
    # Plot 2: Scatter plot of compliance vs latency
    for mode in ['Vanilla', 'Deadline-aware']:
        mode_df = deadline_df[deadline_df['mode'] == mode]
        ax2.scatter(mode_df['avg_latency_ms'], mode_df['compliance_pct'], 
                   label=mode, alpha=0.6, s=100)
    
    ax2.set_title('Compliance vs Latency Trade-off', fontsize=14)
    ax2.set_xlabel('Average Latency (ms)')
    ax2.set_ylabel('Compliance (%)')
    ax2.set_xscale('log')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'deadline_effectiveness.png', dpi=300, bbox_inches='tight')
    print("Saved: deadline_effectiveness.png")

def generate_summary_table(df, output_dir):
    """Generate a summary table of key metrics."""
    summary_data = []
    
    for scenario in df['scenario'].unique():
        for mode in ['Vanilla', 'Deadline-aware']:
            scenario_mode_df = df[(df['scenario'] == scenario) & (df['mode'] == mode)]
            
            if not scenario_mode_df.empty:
                summary_data.append({
                    'Scenario': scenario.replace('_', ' ').title(),
                    'Mode': mode,
                    'Avg Compliance (%)': f"{scenario_mode_df['compliance_pct'].mean():.1f}",
                    'Avg Latency (ms)': f"{scenario_mode_df['avg_latency_ms'].mean():.1f}",
                    'Avg Throughput (Mbps)': f"{scenario_mode_df['throughput_mbps'].mean():.2f}",
                    'Tests Run': len(scenario_mode_df)
                })
    
    summary_df = pd.DataFrame(summary_data)
    
    # Save as CSV
    summary_df.to_csv(output_dir / 'summary_table.csv', index=False)
    print("Saved: summary_table.csv")
    
    # Display summary
    print("\n=== Performance Summary ===")
    print(summary_df.to_string(index=False))

def plot_improvement_metrics(df, output_dir):
    """Plot improvement metrics of deadline-aware over vanilla."""
    improvements = []
    
    for scenario in df['scenario'].unique():
        vanilla_df = df[(df['scenario'] == scenario) & (df['mode'] == 'Vanilla')]
        deadline_df = df[(df['scenario'] == scenario) & (df['mode'] == 'Deadline-aware')]
        
        if not vanilla_df.empty and not deadline_df.empty:
            # Calculate average improvements
            compliance_imp = (deadline_df['compliance_pct'].mean() - 
                            vanilla_df['compliance_pct'].mean())
            
            # For latency, negative is better (reduction)
            latency_imp = -((deadline_df['avg_latency_ms'].mean() - 
                           vanilla_df['avg_latency_ms'].mean()) / 
                          vanilla_df['avg_latency_ms'].mean() * 100)
            
            throughput_imp = ((deadline_df['throughput_mbps'].mean() - 
                             vanilla_df['throughput_mbps'].mean()) / 
                            vanilla_df['throughput_mbps'].mean() * 100)
            
            improvements.append({
                'scenario': scenario.replace('_', ' ').title(),
                'Compliance\nImprovement (pp)': compliance_imp,
                'Latency\nReduction (%)': latency_imp,
                'Throughput\nImprovement (%)': throughput_imp
            })
    
    imp_df = pd.DataFrame(improvements)
    
    # Create bar plot
    fig, ax = plt.subplots(figsize=(12, 8))
    
    x = np.arange(len(imp_df))
    width = 0.25
    
    bars1 = ax.bar(x - width, imp_df['Compliance\nImprovement (pp)'], 
                    width, label='Compliance (pp)', color='green', alpha=0.7)
    bars2 = ax.bar(x, imp_df['Latency\nReduction (%)'], 
                    width, label='Latency Reduction (%)', color='blue', alpha=0.7)
    bars3 = ax.bar(x + width, imp_df['Throughput\nImprovement (%)'], 
                    width, label='Throughput (%)', color='orange', alpha=0.7)
    
    ax.set_title('DMTP Performance Improvements over Vanilla QUIC', fontsize=16)
    ax.set_xlabel('Scenario')
    ax.set_ylabel('Improvement (%)')
    ax.set_xticks(x)
    ax.set_xticklabels(imp_df['scenario'], rotation=45, ha='right')
    ax.legend()
    ax.grid(axis='y', alpha=0.3)
    ax.axhline(y=0, color='black', linestyle='-', linewidth=0.5)
    
    # Add value labels
    for bars in [bars1, bars2, bars3]:
        for bar in bars:
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2., height,
                   f'{height:.1f}', ha='center', va='bottom' if height > 0 else 'top',
                   fontsize=8)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'improvement_metrics.png', dpi=300, bbox_inches='tight')
    print("Saved: improvement_metrics.png")

def main():
    # Check if running from build directory
    if Path.cwd().name == 'build':
        csv_path = Path('comprehensive_multipath_eval_results.csv')
    else:
        csv_path = Path('build/comprehensive_multipath_eval_results.csv')
    
    if not csv_path.exists():
        print(f"Error: Could not find {csv_path}")
        print("Please run the multipath_comprehensive_eval test first.")
        sys.exit(1)
    
    # Create output directory for graphs
    output_dir = Path('multipath_eval_graphs')
    output_dir.mkdir(exist_ok=True)
    
    # Load data
    df = load_data(csv_path)
    
    # Generate all plots
    print("\nGenerating analysis graphs...")
    plot_compliance_comparison(df, output_dir)
    plot_latency_comparison(df, output_dir)
    plot_network_impact(df, output_dir)
    plot_path_utilization(df, output_dir)
    plot_deadline_effectiveness(df, output_dir)
    plot_improvement_metrics(df, output_dir)
    
    # Generate summary table
    generate_summary_table(df, output_dir)
    
    print(f"\nAll graphs saved to: {output_dir}/")
    print("\nAnalysis complete!")

if __name__ == "__main__":
    main()