#!/usr/bin/env python3
"""
plot_analysis.py — visualise Puppetmaster analysis results.

Typical pipeline
----------------
    ./analyze_v2 txns.csv run.log 8 50 \
        | python3 plot_analysis.py -o report.pdf \
        2>summary.txt

Stdout from `analyze_v2` must contain the three blocks:

    # LATENCY_CDF
    # THROUGHPUT_TS
    # PUPPET_UTIL

Each block is valid CSV headed by its own line that starts with '# '.
"""

import sys
import io
import argparse
import re

import pandas as pd
import matplotlib.pyplot as plt

plt.rcParams["figure.autolayout"] = True
plt.rcParams["axes.grid"] = True


# ----------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------
def split_blocks(raw: str):
    """
    Return dict {header → CSV-string}.
    A block starts with a line '# HEADER' and continues until the next '# '.
    """
    header = None
    buf = []
    out = {}

    for line in raw.splitlines():
        if line.startswith("# "):
            if header and buf:
                out[header] = "\n".join(buf).strip()
            header = line[2:].strip()
            buf = []
        else:
            buf.append(line)
    if header and buf:
        out[header] = "\n".join(buf).strip()
    return out


def plot_latency_cdf(df: pd.DataFrame, ax):
    ax.plot(df["lat_us"], df["cdf_pct"], marker=".")
    ax.set_xscale("log")
    ax.set_xlabel("Latency (µs, log scale)")
    ax.set_ylabel("CDF %")
    ax.set_title("End-to-end latency CDF")
    ax.set_ylim(0, 100)


def plot_throughput(df: pd.DataFrame, ax):
    ax.plot(df["time_ms"], df["thr_txn_per_s"])
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Throughput (txn/s)")
    window = df["time_ms"].diff().median()
    ax.set_title(f"Throughput vs time (window ≈ {window:.0f} ms)")


def plot_util(df: pd.DataFrame, ax):
    ax.bar(df["puppet_id"], df["util_pct"])
    ax.set_xlabel("Puppet ID")
    ax.set_ylabel("Utilisation %")
    ax.set_title("Per-puppet utilisation")
    ax.set_ylim(0, 100)


# ----------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Generate latency/throughput/utilisation plots from analyze_v2."
    )
    ap.add_argument(
        "-o",
        "--out",
        required=True,
        metavar="FILE",
        help="output file (extension decides format: .pdf, .png, etc.)",
    )
    args = ap.parse_args()

    raw = sys.stdin.read()
    blocks = split_blocks(raw)

    # mandatory blocks
    need = ["LATENCY_CDF", "THROUGHPUT_TS", "PUPPET_UTIL"]
    for h in need:
        if h not in blocks:
            sys.stderr.write(f"Error: missing CSV block '{h}' in stdin\n")
            sys.exit(1)

    lat = pd.read_csv(io.StringIO(blocks["LATENCY_CDF"]))
    thr = pd.read_csv(io.StringIO(blocks["THROUGHPUT_TS"]))
    util = pd.read_csv(io.StringIO(blocks["PUPPET_UTIL"]))

    # figure
    fig, axes = plt.subplots(3, 1, figsize=(6, 10))
    plot_latency_cdf(lat, axes[0])
    plot_throughput(thr, axes[1])
    plot_util(util, axes[2])

    fig.savefig(args.out)
    print(f"[plot_analysis] saved → {args.out}")


# ----------------------------------------------------------------------
if __name__ == "__main__":
    main()

