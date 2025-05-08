#!/usr/bin/env python3
"""
Script to read the binary output from analyze.c and print it in human-readable format.
This is a sanity check before implementing graphing functionality.
"""

import struct
import sys
from typing import Dict, List, Tuple, Any


def read_binary_output(filename: str) -> Dict[str, Any]:
    """
    Read the binary output file from analyze.c and parse it into a dictionary.
    """
    data = {}
    
    with open(filename, 'rb') as f:
        # Read header information
        data['total_txns'] = struct.unpack('i', f.read(4))[0]
        data['complete_txns'] = struct.unpack('i', f.read(4))[0]
        data['filtered_count'] = struct.unpack('i', f.read(4))[0]
        data['num_puppets'] = struct.unpack('i', f.read(4))[0]
        data['average_throughput'] = struct.unpack('d', f.read(8))[0]
        
        # Read throughput window information
        data['num_throughput_windows'] = struct.unpack('i', f.read(4))[0]
        data['window_seconds'] = struct.unpack('d', f.read(8))[0]
        
        # Read windowed throughput data
        n_windows = data['num_throughput_windows']
        
        # For each stage, read pairs of (time, throughput)
        stages = ['submit', 'sched', 'recv', 'done', 'cleanup']
        for stage in stages:
            times = []
            throughputs = []
            for _ in range(n_windows):
                times.append(struct.unpack('d', f.read(8))[0])
                throughputs.append(struct.unpack('d', f.read(8))[0])
            data[f'{stage}_throughput'] = {
                'times': times,
                'values': throughputs
            }
        
        # Read histogram information
        data['num_buckets'] = struct.unpack('i', f.read(4))[0]
        n_buckets = data['num_buckets']
        
        # Read histograms for each latency type
        latency_types = ['e2e', 'submit_sched', 'sched_recv', 'recv_done', 'done_cleanup']
        for lt in latency_types:
            centers = []
            counts = []
            cdfs = []
            for _ in range(n_buckets):
                centers.append(struct.unpack('d', f.read(8))[0])
                counts.append(struct.unpack('i', f.read(4))[0])
                cdfs.append(struct.unpack('d', f.read(8))[0])
            data[f'{lt}_histogram'] = {
                'centers': centers,
                'counts': counts,
                'cdfs': cdfs
            }
    
    return data


def print_data(data: Dict[str, Any]) -> None:
    """
    Print the parsed data in a human-readable format.
    """
    print("======== SUMMARY INFORMATION ========")
    print(f"Total transactions: {data['total_txns']}")
    print(f"Complete transactions: {data['complete_txns']}")
    print(f"Filtered transactions (after outlier removal): {data['filtered_count']}")
    print(f"Number of puppets: {data['num_puppets']}")
    print(f"Average throughput: {data['average_throughput']:.2f} txn/s")
    print(f"Number of throughput windows: {data['num_throughput_windows']}")
    print(f"Window size: {data['window_seconds']:.6f} seconds")
    print(f"Number of histogram buckets: {data['num_buckets']}")
    print()
    
    print("======== THROUGHPUT DATA ========")
    stages = ['submit', 'sched', 'recv', 'done', 'cleanup']
    for stage in stages:
        throughput_data = data[f'{stage}_throughput']
        min_tp = min(throughput_data['values'])
        max_tp = max(throughput_data['values'])
        avg_tp = sum(throughput_data['values']) / len(throughput_data['values'])
        print(f"{stage.capitalize()} throughput: min={min_tp:.2f}, max={max_tp:.2f}, avg={avg_tp:.2f} txn/s")
        
        # Print first and last few windows
        print("  First 3 windows:")
        for i in range(min(3, len(throughput_data['times']))):
            print(f"    Time {throughput_data['times'][i]:.3f}s: {throughput_data['values'][i]:.2f} txn/s")
        
        print("  Last 3 windows:")
        for i in range(max(0, len(throughput_data['times'])-3), len(throughput_data['times'])):
            print(f"    Time {throughput_data['times'][i]:.3f}s: {throughput_data['values'][i]:.2f} txn/s")
        print()
    
    print("======== LATENCY HISTOGRAMS ========")
    latency_types = [
        ('e2e', 'End-to-end (Submit to Done)'),
        ('submit_sched', 'Submit to Schedule'),
        ('sched_recv', 'Schedule to Work Received'),
        ('recv_done', 'Work Received to Done'),
        ('done_cleanup', 'Done to Cleanup')
    ]
    
    for lt_key, lt_name in latency_types:
        hist_data = data[f'{lt_key}_histogram']
        # Find non-zero buckets
        non_zero = [i for i, count in enumerate(hist_data['counts']) if count > 0]
        if non_zero:
            min_idx, max_idx = min(non_zero), max(non_zero)
            min_latency = hist_data['centers'][min_idx]
            max_latency = hist_data['centers'][max_idx]
            
            # Find peak
            peak_idx = hist_data['counts'].index(max(hist_data['counts']))
            peak_latency = hist_data['centers'][peak_idx]
            
            print(f"{lt_name} latency:")
            print(f"  Range: {min_latency*1e6:.2f} µs to {max_latency*1e6:.2f} µs")
            print(f"  Peak at: {peak_latency*1e6:.2f} µs (count: {hist_data['counts'][peak_idx]})")
            
            # Print first and last few buckets with non-zero values
            print("  First 3 non-empty buckets:")
            for i in range(min_idx, min(min_idx+3, data['num_buckets'])):
                if hist_data['counts'][i] > 0:
                    print(f"    {hist_data['centers'][i]*1e6:.2f} µs: count={hist_data['counts'][i]}, CDF={hist_data['cdfs'][i]:.3f}")
            
            print("  Last 3 non-empty buckets:")
            for i in range(max(min_idx, max_idx-2), max_idx+1):
                if hist_data['counts'][i] > 0:
                    print(f"    {hist_data['centers'][i]*1e6:.2f} µs: count={hist_data['counts'][i]}, CDF={hist_data['cdfs'][i]:.3f}")
        else:
            print(f"{lt_name} latency: No data")
        print()


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} out.bin")
        return 1
    
    binary_file = sys.argv[1]
    try:
        data = read_binary_output(binary_file)
        print_data(data)
    except Exception as e:
        print(f"Error reading binary file: {e}")
        return 1
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
