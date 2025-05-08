#!/usr/bin/env python3
"""
Script to read the binary output from analyze.c and create visualizations.
Generates SVG files for individual graphs and a PDF report containing all graphs.
"""

import struct
import sys
import os
from typing import Dict, List, Tuple, Any
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.backends.backend_pdf import PdfPages
import numpy as np

# Stage colors for consistency across all graphs
COLORS = {
    'submit': '#1f77b4',    # blue
    'sched': '#ff7f0e',     # orange
    'recv': '#2ca02c',      # green
    'done': '#d62728',      # red
    'cleanup': '#9467bd',   # purple
    'e2e': '#000000',       # black
}

# Stage names for legends and labels
STAGE_NAMES = {
    'submit': 'Submit',
    'sched': 'Schedule',
    'recv': 'Work Received',
    'done': 'Done',
    'cleanup': 'Cleanup',
    'e2e': 'End-to-End',
    'submit_sched': 'Submit to Schedule',
    'sched_recv': 'Schedule to Work',
    'recv_done': 'Work to Done',
    'done_cleanup': 'Done to Cleanup',
}

# Figure sizes and DPI
FIG_SIZE = (10, 6)
DPI = 100

# Time unit definitions and conversion factors
TIME_UNITS = {
    0: {'name': 'ns', 'factor': 1e9, 'label': 'Nanoseconds'},
    1: {'name': 'μs', 'factor': 1e6, 'label': 'Microseconds'},
    2: {'name': 'ms', 'factor': 1e3, 'label': 'Milliseconds'},
    3: {'name': 's',  'factor': 1.0, 'label': 'Seconds'}
}

def convert_time_with_unit(time_seconds, unit_id):
    """
    Convert time from seconds to the specified unit.
    Returns the value and unit string.
    """
    unit = TIME_UNITS[unit_id]
    value = time_seconds * unit['factor']
    return value, unit['name'], unit['label']


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
        data['num_buckets'] = struct.unpack('i', f.read(4))[0]
        data['cpu_freq'] = struct.unpack('d', f.read(8))[0]
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
        latency_types = ['e2e', 'submit_sched', 'sched_recv', 'recv_done', 'done_cleanup']
        # Read time units for each histogram
        for lt in latency_types:
            unit_id = struct.unpack('i', f.read(4))[0]
            if unit_id not in TIME_UNITS:
                unit_id = 1  # Default to microseconds if invalid
            data[f'{lt}_unit'] = unit_id
        
        n_buckets = data['num_buckets']
        
        # Read histograms for each latency type
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
                'cdfs': cdfs,
                'unit': data[f'{lt}_unit']
            }
    
    return data


def plot_throughput_individual(data: Dict[str, Any], stage: str, output_file: str) -> None:
    """
    Plot throughput over time for a single stage and save as SVG.
    """
    plt.figure(figsize=FIG_SIZE, dpi=DPI)
    
    throughput_data = data[f'{stage}_throughput']
    
    plt.plot(
        throughput_data['times'],
        throughput_data['values'],
        color=COLORS[stage],
        linewidth=2,
        label=STAGE_NAMES[stage]
    )
    
    # Add average line
    plt.axhline(y=data['average_throughput'], color='r', linestyle='--', 
                label=f'Avg: {data["average_throughput"]:.2f} txn/s')
    
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.xlabel('Time (seconds)', fontsize=12)
    plt.ylabel('Throughput (txn/s)', fontsize=12)
    plt.title(f'{STAGE_NAMES[stage]} Throughput Over Time', fontsize=14)
    plt.legend(fontsize=10)
    
    # Save to file
    plt.tight_layout()
    plt.savefig(output_file)
    plt.close()
    
    print(f"Generated: {output_file}")


def plot_throughput_combined(data: Dict[str, Any], output_file: str) -> None:
    """
    Plot throughput over time for all stages on the same graph and save as SVG.
    """
    plt.figure(figsize=FIG_SIZE, dpi=DPI)
    
    stages = ['submit', 'sched', 'recv', 'done', 'cleanup']
    
    # Find global y-axis limits
    max_throughput = 0
    for stage in stages:
        throughput_data = data[f'{stage}_throughput']
        max_throughput = max(max_throughput, max(throughput_data['values']))
    
    for stage in stages:
        throughput_data = data[f'{stage}_throughput']
        plt.plot(
            throughput_data['times'],
            throughput_data['values'],
            color=COLORS[stage],
            linewidth=2,
            label=STAGE_NAMES[stage]
        )
    
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.xlabel('Time (seconds)', fontsize=12)
    plt.ylabel('Throughput (txn/s)', fontsize=12)
    plt.title('Combined Throughput Over Time', fontsize=14)
    plt.legend(fontsize=10)
    
    # Use consistent y-axis
    plt.ylim(0, max_throughput * 1.1)
    
    # Save to file
    plt.tight_layout()
    plt.savefig(output_file)
    plt.close()
    
    print(f"Generated: {output_file}")


def plot_latency_histogram(data: Dict[str, Any], latency_type: str, output_file: str) -> None:
    """
    Plot latency histogram with CDF overlay for a specific latency type and save as SVG.
    """
    # Get histogram data and time unit
    hist_data = data[f'{latency_type}_histogram']
    unit_id = hist_data['unit']
    
    # Create figure with two y-axes
    fig, ax1 = plt.subplots(figsize=FIG_SIZE, dpi=DPI)
    ax2 = ax1.twinx()
    
    # Convert centers to appropriate unit
    centers_converted = []
    unit_str = ""
    unit_label = ""
    
    for center in hist_data['centers']:
        val, unit_str, unit_label = convert_time_with_unit(center, unit_id)
        centers_converted.append(val)
    
    # Plot histogram bars
    color = COLORS[latency_type] if latency_type in COLORS else COLORS['e2e']
    bar_width = centers_converted[1]-centers_converted[0] if len(centers_converted) > 1 else 1
    ax1.bar(centers_converted, hist_data['counts'], width=bar_width,
            alpha=0.6, color=color, label='Frequency')
    
    # Plot CDF line
    ax2.plot(centers_converted, hist_data['cdfs'], 'r-', linewidth=2, label='CDF')
    
    # Add markers for key percentiles (50%, 95%, 99%)
    percentiles = [0.5, 0.95, 0.99]
    for p in percentiles:
        idx = next((i for i, cdf in enumerate(hist_data['cdfs']) if cdf >= p), len(hist_data['cdfs'])-1)
        if idx < len(centers_converted):
            x = centers_converted[idx]
            y = hist_data['cdfs'][idx]
            ax2.plot(x, y, 'ro', markersize=5)
            ax2.annotate(f'{int(p*100)}%', 
                        xy=(x, y),
                        xytext=(10, -10 if p < 0.9 else 10), 
                        textcoords='offset points',
                        arrowprops=dict(arrowstyle='->', connectionstyle='arc3,rad=.2'),
                        fontsize=8)
    
    # Set labels and title
    ax1.set_xlabel(f'Latency ({unit_str})', fontsize=12)
    ax1.set_ylabel('Frequency', fontsize=12)
    ax2.set_ylabel('CDF', fontsize=12)
    
    # Get the stage name
    stage_name = STAGE_NAMES.get(latency_type, latency_type)
    plt.title(f'{stage_name} Latency Distribution', fontsize=14)
    
    # Add grid to histogram
    ax1.grid(True, linestyle='--', alpha=0.7)
    
    # Set y-axis limits for CDF
    ax2.set_ylim(0, 1.05)
    
    # Add legends
    ax1.legend(loc='upper left', fontsize=10)
    ax2.legend(loc='lower right', fontsize=10)
    
    # Save to file
    plt.tight_layout()
    plt.savefig(output_file)
    plt.close()
    
    print(f"Generated: {output_file}")


def generate_pdf_report(data: Dict[str, Any], output_file: str) -> None:
    """
    Generate a PDF report containing all visualization graphs.
    """
    with PdfPages(output_file) as pdf:
        # First page: End-to-end latency histogram and end-to-end throughput
        plt.figure(figsize=(11, 8.5), dpi=DPI)
        plt.suptitle('Performance Overview', fontsize=16)
        
        # End-to-end latency histogram
        plt.subplot(2, 1, 1)
        
        hist_data = data['e2e_histogram']
        unit_id = hist_data['unit']
        
        centers_converted = []
        for center in hist_data['centers']:
            val, unit_str, unit_label = convert_time_with_unit(center, unit_id)
            centers_converted.append(val)
        
        ax1 = plt.gca()
        ax2 = ax1.twinx()
        
        bar_width = centers_converted[1]-centers_converted[0] if len(centers_converted) > 1 else 1
        ax1.bar(centers_converted, hist_data['counts'], width=bar_width,
                alpha=0.6, color=COLORS['e2e'], label='Frequency')
        ax2.plot(centers_converted, hist_data['cdfs'], 'r-', linewidth=2, label='CDF')
        
        # Add percentile markers
        percentiles = [0.5, 0.95, 0.99]
        for p in percentiles:
            idx = next((i for i, cdf in enumerate(hist_data['cdfs']) if cdf >= p), len(hist_data['cdfs'])-1)
            if idx < len(centers_converted):
                x = centers_converted[idx]
                y = hist_data['cdfs'][idx]
                ax2.plot(x, y, 'ro', markersize=5)
                ax2.annotate(f'{int(p*100)}%', 
                            xy=(x, y),
                            xytext=(10, -10 if p < 0.9 else 10), 
                            textcoords='offset points',
                            arrowprops=dict(arrowstyle='->', connectionstyle='arc3,rad=.2'),
                            fontsize=8)
        
        ax1.set_xlabel(f'Latency ({unit_str})', fontsize=12)
        ax1.set_ylabel('Frequency', fontsize=12)
        ax2.set_ylabel('CDF', fontsize=12)
        ax2.set_ylim(0, 1.05)
        plt.title('End-to-End Latency Distribution', fontsize=14)
        ax1.legend(loc='upper left', fontsize=10)
        ax2.legend(loc='lower right', fontsize=10)
        ax1.grid(True, linestyle='--', alpha=0.7)
        
        # End-to-end throughput (using 'done' stage)
        plt.subplot(2, 1, 2)
        throughput_data = data['done_throughput']
        plt.plot(throughput_data['times'], throughput_data['values'], 
                 color=COLORS['done'], linewidth=2, label='Throughput')
        plt.axhline(y=data['average_throughput'], color='r', linestyle='--', 
                    label=f'Avg: {data["average_throughput"]:.2f} txn/s')
        
        plt.grid(True, linestyle='--', alpha=0.7)
        plt.xlabel('Time (seconds)', fontsize=12)
        plt.ylabel('Throughput (txn/s)', fontsize=12)
        plt.title('End-to-End Throughput Over Time', fontsize=14)
        plt.legend(fontsize=10)
        
        plt.tight_layout(rect=[0, 0, 1, 0.95])  # Adjust layout accounting for suptitle
        pdf.savefig()
        plt.close()
        
        # Page for each stage latency histogram
        latency_types = [
            ('submit_sched', 'Submit to Schedule'),
            ('sched_recv', 'Schedule to Work'),
            ('recv_done', 'Work to Done'),
            ('done_cleanup', 'Done to Cleanup')
        ]
        
        for lt_key, lt_name in latency_types:
            plt.figure(figsize=(11, 8.5), dpi=DPI)
            plt.suptitle(f'{lt_name} Performance', fontsize=16)
            
            # Latency histogram
            hist_data = data[f'{lt_key}_histogram']
            unit_id = hist_data['unit']
            centers_converted = []
            for center in hist_data['centers']:
                val, unit_str, unit_label = convert_time_with_unit(center, unit_id)
                centers_converted.append(val)
            
            ax1 = plt.gca()
            ax2 = ax1.twinx()
            
            color = COLORS[lt_key.split('_')[0]]  # Use color of first stage in pair
            bar_width = centers_converted[1]-centers_converted[0] if len(centers_converted) > 1 else 1
            ax1.bar(centers_converted, hist_data['counts'], width=bar_width,
                    alpha=0.6, color=color, label='Frequency')
            ax2.plot(centers_converted, hist_data['cdfs'], 'r-', linewidth=2, label='CDF')
            
            # Add percentile markers
            percentiles = [0.5, 0.95, 0.99]
            for p in percentiles:
                idx = next((i for i, cdf in enumerate(hist_data['cdfs']) if cdf >= p), len(hist_data['cdfs'])-1)
                if idx < len(centers_converted):
                    x = centers_converted[idx]
                    y = hist_data['cdfs'][idx]
                    ax2.plot(x, y, 'ro', markersize=5)
                    ax2.annotate(f'{int(p*100)}%', 
                                xy=(x, y),
                                xytext=(10, -10 if p < 0.9 else 10), 
                                textcoords='offset points',
                                arrowprops=dict(arrowstyle='->', connectionstyle='arc3,rad=.2'),
                                fontsize=8)
            
            ax1.set_xlabel(f'Latency ({unit_str})', fontsize=12)
            ax1.set_ylabel('Frequency', fontsize=12)
            ax2.set_ylabel('CDF', fontsize=12)
            ax2.set_ylim(0, 1.05)
            plt.title(f'{lt_name} Latency Distribution', fontsize=14)
            ax1.legend(loc='upper left', fontsize=10)
            ax2.legend(loc='lower right', fontsize=10)
            ax1.grid(True, linestyle='--', alpha=0.7)
            
            plt.tight_layout(rect=[0, 0, 1, 0.95])
            pdf.savefig()
            plt.close()
        
        # Combined throughput graph page
        plt.figure(figsize=(11, 8.5), dpi=DPI)
        plt.suptitle('Throughput Analysis', fontsize=16)
        
        stages = ['submit', 'sched', 'recv', 'done', 'cleanup']
        max_throughput = 0
        for stage in stages:
            throughput_data = data[f'{stage}_throughput']
            max_throughput = max(max_throughput, max(throughput_data['values']))
            plt.plot(
                throughput_data['times'],
                throughput_data['values'],
                color=COLORS[stage],
                linewidth=2,
                label=STAGE_NAMES[stage]
            )
        
        plt.grid(True, linestyle='--', alpha=0.7)
        plt.xlabel('Time (seconds)', fontsize=12)
        plt.ylabel('Throughput (txn/s)', fontsize=12)
        plt.title('Throughput by Stage', fontsize=14)
        plt.legend(fontsize=10)
        plt.ylim(0, max_throughput * 1.1)
        
        plt.tight_layout(rect=[0, 0, 1, 0.95])
        pdf.savefig()
        plt.close()
        
        # Add summary statistics page
        plt.figure(figsize=(11, 8.5), dpi=DPI)
        plt.axis('off')
        plt.suptitle('Performance Summary Statistics', fontsize=16)
        
        summary_text = f"""
        Total Transactions: {data['total_txns']}
        Complete Transactions: {data['complete_txns']}
        Filtered Transactions (after outlier removal): {data['filtered_count']}
        Number of Puppets: {data['num_puppets']}
        
        Average Throughput: {data['average_throughput']:,.2f} txn/s
        
        Latency Statistics:
        """
        
        latency_types = ['e2e', 'submit_sched', 'sched_recv', 'recv_done', 'done_cleanup']
        for lt in latency_types:
            hist_data = data[f'{lt}_histogram']
            unit_id = hist_data['unit']
            counts = hist_data['counts']
            
            # Find non-zero buckets for actual min/max
            non_zero_indices = [i for i, count in enumerate(counts) if count > 0]
            if non_zero_indices:
                min_val, unit_str, unit_label = convert_time_with_unit(hist_data['centers'][min(non_zero_indices)], unit_id)
                max_val, _, _ = convert_time_with_unit(hist_data['centers'][max(non_zero_indices)], unit_id)
                
                # Calculate weighted average for mean latency
                total_count = sum(counts)
                if total_count > 0:
                    mean_val = sum(convert_time_with_unit(hist_data['centers'][i], unit_id)[0] * counts[i] 
                                 for i in range(len(hist_data['centers']))) / total_count
                    
                    # Find key percentiles (50%, 95%, 99%)
                    cdfs = hist_data['cdfs']
                    median_idx = next((i for i, cdf in enumerate(cdfs) if cdf >= 0.5), len(cdfs)-1)
                    median_val = convert_time_with_unit(hist_data['centers'][median_idx], unit_id)[0]
                    
                    # Find 95th percentile
                    p95_idx = next((i for i, cdf in enumerate(cdfs) if cdf >= 0.95), len(cdfs)-1)
                    p95_val = convert_time_with_unit(hist_data['centers'][p95_idx], unit_id)[0]
                    
                    # Find 99th percentile
                    p99_idx = next((i for i, cdf in enumerate(cdfs) if cdf >= 0.99), len(cdfs)-1)
                    p99_val = convert_time_with_unit(hist_data['centers'][p99_idx], unit_id)[0]
                    
                    # Calculate standard deviation and coefficient of variation
                    variance = sum(((convert_time_with_unit(hist_data['centers'][i], unit_id)[0] - mean_val) ** 2) * counts[i] 
                                  for i in range(len(hist_data['centers']))) / total_count
                    std_dev = variance ** 0.5
                    cv = (std_dev / mean_val * 100) if mean_val > 0 else 0  # as percentage
                    
                    stage_name = STAGE_NAMES.get(lt, lt)
                    summary_text += f"""
        {stage_name}:
          Min: {min_val:.2f} {unit_str}
          Mean: {mean_val:.2f} {unit_str}
          Median: {median_val:.2f} {unit_str}
          95th Percentile: {p95_val:.2f} {unit_str}
          99th Percentile: {p99_val:.2f} {unit_str}
          Max: {max_val:.2f} {unit_str}
          Std Dev: {std_dev:.2f} {unit_str}
          Variability: {cv:.1f}%
                    """
                else:
                    summary_text += f"\n        {STAGE_NAMES.get(lt, lt)}: No data\n"
            else:
                summary_text += f"\n        {STAGE_NAMES.get(lt, lt)}: No data\n"
        
        plt.text(0.1, 0.9, summary_text, fontsize=12, verticalalignment='top',
                 horizontalalignment='left', transform=plt.gca().transAxes)
        
        pdf.savefig()
        plt.close()
    
    print(f"Generated: {output_file}")


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} analyzed.bin")
        return 1
    
    binary_file = sys.argv[1]
    
    try:
        print(f"Reading binary data from {binary_file}...")
        data = read_binary_output(binary_file)
        
        # Create output directory if it doesn't exist
        os.makedirs("output", exist_ok=True)
        
        # Generate individual throughput graphs
        stages = ['submit', 'sched', 'recv', 'done', 'cleanup']
        for stage in stages:
            output_file = f"output/throughput-{stage}.svg"
            plot_throughput_individual(data, stage, output_file)
        
        # Generate combined throughput graph
        plot_throughput_combined(data, "output/throughput-combined.svg")
        
        # Generate latency histograms
        latency_types = ['e2e', 'submit_sched', 'sched_recv', 'recv_done', 'done_cleanup']
        for lt in latency_types:
            output_file = f"output/latency-{lt}.svg"
            plot_latency_histogram(data, lt, output_file)
        
        # Generate PDF report with all visualizations
        generate_pdf_report(data, "output/report.pdf")
        
        print("All visualizations generated successfully!")
        
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return 1
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
