#!/usr/bin/env python3
"""
plot_analysis.py — visualise Puppetmaster analysis output.

Typical usage
-------------
    ./analyze_v2 txns.csv run.log 8 50 \
        | python3 plot_analysis.py -o report.pdf \
        2>summary.txt

• Reads CSV blocks from stdin, separated by lines that begin '# '.
• Required blocks: LATENCY_CDF, LATENCY_HIST, THROUGHPUT_TS, PUPPET_UTIL
• The THROUGHPUT_TS header may carry metadata:
      "# THROUGHPUT_TS slide_ms=12.5"
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
def split_blocks(raw: str):
    """
    Return dict {key : (csv_text, meta_dict)}.
      * key  : first token after '# '
      * meta : key=value pairs found on the remainder of the header line
    """
    blocks = {}
    key, buf, meta = None, [], {}

    kv_re = re.compile(r"(\w+)=([\d\.]+)")

    for line in raw.splitlines():
        if line.startswith("# "):
            if key and buf:
                blocks[key] = ("\n".join(buf).strip(), meta)
            tokens = line[2:].strip().split(maxsplit=1)
            key = tokens[0]
            meta = {}
            if len(tokens) == 2:               # parse k=v k2=v2 ...
                for m in kv_re.finditer(tokens[1]):
                    meta[m.group(1)] = float(m.group(2))
            buf = []
        else:
            buf.append(line)
    if key and buf:
        blocks[key] = ("\n".join(buf).strip(), meta)
    return blocks


# ----------------------------------------------------------------------
def bars_width(df):
    """Return an array of bar widths for histogram bars."""
    if len(df) == 1:
        return df["lat_us"].iloc[0] * 0.1 or 0.1
    return df["lat_us"].diff().fillna(df["lat_us"].diff().iloc[1])


# ---- Plot helpers ---------------------------------------------------
def plot_latency(hist_df, cdf_df, ax):
    ax.bar(hist_df["lat_us"], hist_df["count"],
           width=bars_width(hist_df), alpha=0.3, label="histogram")
    ax.set_xlabel("Latency (µs)")
    ax.set_ylabel("Count (bars)")

    ax2 = ax.twinx()
    ax2.plot(cdf_df["lat_us"], cdf_df["cdf_pct"],
             color="tab:red", label="CDF")
    ax2.set_ylabel("CDF %")
    ax2.set_ylim(0, 100)

    ax.set_title("End-to-end latency distribution")
    ax.legend(loc="upper left")
    ax2.legend(loc="upper right")


def plot_throughput(ts_df, ax, slide_ms_meta):
    ax.plot(ts_df["time_ms"], ts_df["thr_txn_per_s"])
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Throughput (txn/s)")

    if slide_ms_meta is not None:
        win = slide_ms_meta
    elif len(ts_df) > 1:
        win = ts_df["time_ms"].diff().median()
    else:
        win = float("nan")
    ax.set_title(f"Throughput (window ≈ {win:.1f} ms)")


def plot_util(util_df, ax):
    ax.bar(util_df["puppet_id"], util_df["util_pct"])
    ax.set_xlabel("Puppet ID")
    ax.set_ylabel("Utilisation %")
    ax.set_ylim(0, 100)
    ax.set_title("Per-puppet utilisation")


# ----------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Generate PDF/PNG from analyze_v2 output")
    ap.add_argument("-o", "--out", required=True,
                    help="output file (.pdf, .png, etc.)")
    args = ap.parse_args()

    blocks = split_blocks(sys.stdin.read())

    # required keys
    needed = ["LATENCY_CDF", "LATENCY_HIST", "THROUGHPUT_TS", "PUPPET_UTIL"]
    missing = [k for k in needed if k not in blocks]
    if missing:
        sys.exit(f"Error: missing CSV block(s): {', '.join(missing)}")

    cdf_csv, _          = blocks["LATENCY_CDF"]
    hist_csv, _         = blocks["LATENCY_HIST"]
    thr_csv, thr_meta   = blocks["THROUGHPUT_TS"]
    util_csv, _         = blocks["PUPPET_UTIL"]

    cdf_df  = pd.read_csv(io.StringIO(cdf_csv))
    hist_df = pd.read_csv(io.StringIO(hist_csv))
    thr_df  = pd.read_csv(io.StringIO(thr_csv))
    util_df = pd.read_csv(io.StringIO(util_csv))

    slide_ms_meta = thr_meta.get("slide_ms")

    fig, axs = plt.subplots(3, 1, figsize=(6, 10))
    plot_latency(hist_df, cdf_df, axs[0])
    plot_throughput(thr_df, axs[1], slide_ms_meta)
    plot_util(util_df, axs[2])

    fig.savefig(args.out)
    print(f"[plot_analysis] saved → {args.out}")


# ----------------------------------------------------------------------
if __name__ == "__main__":
    main()

