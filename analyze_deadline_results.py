#!/usr/bin/env python3
"""
Deadline-Aware Streams Test Result Analyzer

This script analyzes logs and QLOG files from deadline tests to generate
comprehensive reports on deadline performance.
"""

import json
import re
import sys
import os
import argparse
from datetime import datetime
from collections import defaultdict
import matplotlib.pyplot as plt
import numpy as np

class DeadlineAnalyzer:
    def __init__(self, log_dir, qlog_dir=None):
        self.log_dir = log_dir
        self.qlog_dir = qlog_dir
        self.streams = defaultdict(dict)
        self.deadline_events = []
        self.gap_events = []
        self.completion_times = {}
        
    def parse_client_log(self, log_file):
        """Parse client log for deadline-related events"""
        stream_pattern = r"Set deadline on stream (\d+): (\d+) ms \((soft|hard)\)"
        gap_pattern = r"Stream (\d+): Dropped (\d+) bytes due to deadline"
        complete_pattern = r"Stream (\d+).*completed"
        
        with open(log_file, 'r') as f:
            for line in f:
                # Check for deadline setting
                match = re.search(stream_pattern, line)
                if match:
                    stream_id = int(match.group(1))
                    deadline_ms = int(match.group(2))
                    is_hard = match.group(3) == "hard"
                    
                    self.streams[stream_id]['deadline_ms'] = deadline_ms
                    self.streams[stream_id]['is_hard'] = is_hard
                    self.streams[stream_id]['set_time'] = self._extract_timestamp(line)
                
                # Check for gaps
                match = re.search(gap_pattern, line)
                if match:
                    stream_id = int(match.group(1))
                    bytes_dropped = int(match.group(2))
                    
                    self.gap_events.append({
                        'stream_id': stream_id,
                        'bytes_dropped': bytes_dropped,
                        'time': self._extract_timestamp(line)
                    })
                    
                    if stream_id not in self.streams:
                        self.streams[stream_id] = {}
                    
                    self.streams[stream_id]['bytes_dropped'] = \
                        self.streams[stream_id].get('bytes_dropped', 0) + bytes_dropped
                
                # Check for completion
                match = re.search(complete_pattern, line)
                if match:
                    stream_id = int(match.group(1))
                    self.streams[stream_id]['completed'] = True
                    self.streams[stream_id]['completion_time'] = self._extract_timestamp(line)
    
    def parse_qlog(self, qlog_file):
        """Parse QLOG file for detailed deadline events"""
        try:
            with open(qlog_file, 'r') as f:
                qlog_data = json.load(f)
            
            events = qlog_data.get('traces', [{}])[0].get('events', [])
            
            for event in events:
                event_data = event[-1]  # Event data is last element
                event_type = event_data.get('type', '')
                
                # Look for deadline-related events
                if 'deadline' in event_type.lower():
                    self.deadline_events.append({
                        'time': event[0],
                        'type': event_type,
                        'data': event_data
                    })
                
                # Look for stream events
                if event_type == 'stream_data_blocked':
                    stream_id = event_data.get('stream_id')
                    if stream_id is not None:
                        self.streams[stream_id]['blocked_events'] = \
                            self.streams[stream_id].get('blocked_events', 0) + 1
        
        except Exception as e:
            print(f"Error parsing QLOG {qlog_file}: {e}")
    
    def calculate_metrics(self):
        """Calculate deadline performance metrics"""
        metrics = {
            'total_streams': len(self.streams),
            'streams_with_deadlines': 0,
            'hard_deadlines': 0,
            'soft_deadlines': 0,
            'deadlines_met': 0,
            'total_bytes_dropped': 0,
            'streams_with_drops': 0,
            'completion_rate': 0,
            'avg_deadline_margin': [],
            'deadline_compliance_rate': 0
        }
        
        for stream_id, stream_data in self.streams.items():
            if 'deadline_ms' in stream_data:
                metrics['streams_with_deadlines'] += 1
                
                if stream_data.get('is_hard'):
                    metrics['hard_deadlines'] += 1
                else:
                    metrics['soft_deadlines'] += 1
                
                # Check if deadline was met
                if 'completion_time' in stream_data and 'set_time' in stream_data:
                    duration = stream_data['completion_time'] - stream_data['set_time']
                    deadline = stream_data['deadline_ms']
                    
                    if duration <= deadline:
                        metrics['deadlines_met'] += 1
                        metrics['avg_deadline_margin'].append(deadline - duration)
            
            # Track drops
            if 'bytes_dropped' in stream_data:
                metrics['total_bytes_dropped'] += stream_data['bytes_dropped']
                metrics['streams_with_drops'] += 1
            
            # Track completion
            if stream_data.get('completed', False):
                metrics['completion_rate'] += 1
        
        # Calculate rates
        if metrics['streams_with_deadlines'] > 0:
            metrics['deadline_compliance_rate'] = \
                metrics['deadlines_met'] / metrics['streams_with_deadlines']
        
        if metrics['total_streams'] > 0:
            metrics['completion_rate'] = \
                metrics['completion_rate'] / metrics['total_streams']
        
        if metrics['avg_deadline_margin']:
            metrics['avg_deadline_margin'] = np.mean(metrics['avg_deadline_margin'])
        else:
            metrics['avg_deadline_margin'] = 0
        
        return metrics
    
    def generate_report(self, output_file=None):
        """Generate comprehensive deadline performance report"""
        metrics = self.calculate_metrics()
        
        report = []
        report.append("=" * 60)
        report.append("Deadline-Aware Streams Performance Report")
        report.append("=" * 60)
        report.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        report.append("")
        
        report.append("Stream Statistics:")
        report.append(f"  Total streams: {metrics['total_streams']}")
        report.append(f"  Streams with deadlines: {metrics['streams_with_deadlines']}")
        report.append(f"    - Hard deadlines: {metrics['hard_deadlines']}")
        report.append(f"    - Soft deadlines: {metrics['soft_deadlines']}")
        report.append("")
        
        report.append("Deadline Performance:")
        report.append(f"  Deadlines met: {metrics['deadlines_met']}")
        report.append(f"  Compliance rate: {metrics['deadline_compliance_rate']:.1%}")
        report.append(f"  Average margin: {metrics['avg_deadline_margin']:.1f} ms")
        report.append("")
        
        report.append("Partial Reliability:")
        report.append(f"  Streams with drops: {metrics['streams_with_drops']}")
        report.append(f"  Total bytes dropped: {metrics['total_bytes_dropped']:,}")
        report.append("")
        
        report.append("Completion Statistics:")
        report.append(f"  Completion rate: {metrics['completion_rate']:.1%}")
        report.append("")
        
        # Per-stream details
        if self.streams:
            report.append("Per-Stream Details:")
            report.append("-" * 60)
            report.append(f"{'Stream':<10} {'Deadline':<12} {'Type':<6} {'Status':<12} {'Dropped':<10}")
            report.append("-" * 60)
            
            for stream_id, data in sorted(self.streams.items()):
                deadline = f"{data.get('deadline_ms', 'N/A')} ms" if 'deadline_ms' in data else "No deadline"
                dtype = "Hard" if data.get('is_hard') else "Soft" if 'deadline_ms' in data else "N/A"
                status = "Completed" if data.get('completed') else "Incomplete"
                dropped = f"{data.get('bytes_dropped', 0):,} B"
                
                report.append(f"{stream_id:<10} {deadline:<12} {dtype:<6} {status:<12} {dropped:<10}")
        
        report_text = "\n".join(report)
        
        if output_file:
            with open(output_file, 'w') as f:
                f.write(report_text)
            print(f"Report saved to: {output_file}")
        else:
            print(report_text)
        
        return metrics
    
    def plot_timeline(self, output_file=None):
        """Generate timeline visualization of deadline events"""
        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8))
        
        # Plot 1: Stream deadlines and completions
        stream_ids = []
        deadlines = []
        completions = []
        
        for stream_id, data in self.streams.items():
            if 'deadline_ms' in data:
                stream_ids.append(stream_id)
                deadlines.append(data['deadline_ms'])
                
                if 'completion_time' in data and 'set_time' in data:
                    completion_time = data['completion_time'] - data['set_time']
                    completions.append(completion_time)
                else:
                    completions.append(0)
        
        if stream_ids:
            x = np.arange(len(stream_ids))
            width = 0.35
            
            bars1 = ax1.bar(x - width/2, deadlines, width, label='Deadline', alpha=0.7)
            bars2 = ax1.bar(x + width/2, completions, width, label='Actual', alpha=0.7)
            
            # Color bars based on deadline met/missed
            for i, (d, c) in enumerate(zip(deadlines, completions)):
                if c > 0:  # Has completion time
                    if c <= d:
                        bars2[i].set_color('green')
                    else:
                        bars2[i].set_color('red')
            
            ax1.set_xlabel('Stream ID')
            ax1.set_ylabel('Time (ms)')
            ax1.set_title('Stream Deadlines vs Actual Completion Times')
            ax1.set_xticks(x)
            ax1.set_xticklabels(stream_ids)
            ax1.legend()
            ax1.grid(True, alpha=0.3)
        
        # Plot 2: Gap events over time
        if self.gap_events:
            times = [e['time'] for e in self.gap_events]
            drops = [e['bytes_dropped'] for e in self.gap_events]
            
            ax2.scatter(times, drops, alpha=0.6, s=100)
            ax2.set_xlabel('Time')
            ax2.set_ylabel('Bytes Dropped')
            ax2.set_title('Data Drops Due to Deadline Expiry')
            ax2.grid(True, alpha=0.3)
        
        plt.tight_layout()
        
        if output_file:
            plt.savefig(output_file, dpi=150, bbox_inches='tight')
            print(f"Timeline saved to: {output_file}")
        else:
            plt.show()
    
    def _extract_timestamp(self, line):
        """Extract timestamp from log line"""
        # Simple extraction - can be enhanced based on actual log format
        try:
            match = re.search(r'\[(\d+:\d+:\d+)\]', line)
            if match:
                return match.group(1)
        except:
            pass
        return "00:00:00"

def main():
    parser = argparse.ArgumentParser(description='Analyze deadline test results')
    parser.add_argument('log_dir', help='Directory containing log files')
    parser.add_argument('--qlog-dir', help='Directory containing QLOG files')
    parser.add_argument('--output', '-o', help='Output report file')
    parser.add_argument('--plot', '-p', help='Generate timeline plot')
    parser.add_argument('--client-log', default='client_*.out', 
                        help='Client log file pattern')
    
    args = parser.parse_args()
    
    # Create analyzer
    analyzer = DeadlineAnalyzer(args.log_dir, args.qlog_dir)
    
    # Parse logs
    import glob
    client_logs = glob.glob(os.path.join(args.log_dir, args.client_log))
    
    if not client_logs:
        print(f"No client logs found matching pattern: {args.client_log}")
        return 1
    
    for log_file in client_logs:
        print(f"Parsing: {log_file}")
        analyzer.parse_client_log(log_file)
    
    # Parse QLOGs if available
    if args.qlog_dir:
        qlog_files = glob.glob(os.path.join(args.qlog_dir, '*.qlog'))
        for qlog_file in qlog_files:
            print(f"Parsing QLOG: {qlog_file}")
            analyzer.parse_qlog(qlog_file)
    
    # Generate report
    analyzer.generate_report(args.output)
    
    # Generate plot if requested
    if args.plot:
        analyzer.plot_timeline(args.plot)
    
    return 0

if __name__ == '__main__':
    sys.exit(main())